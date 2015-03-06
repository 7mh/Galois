/** Galois Network Layer for Generalized Buffered Sending -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */

#include "Galois/Runtime/Network.h"
#include "Galois/Runtime/NetworkIO.h"

#include <thread>

using namespace Galois::Runtime;

namespace {

class NetworkInterfaceBuffered : public NetworkInterface {
  static const int COMM_MIN = 1400; // bytes (sligtly smaller than an ethernet packet)
  static const int COMM_DELAY = 100; //microseconds

  Galois::Runtime::NetworkIO* netio;

  struct recvBuffer {
    std::deque<uint8_t> data;
    LL::SimpleLock lock;
    
    std::pair<std::deque<uint8_t>::iterator, std::deque<uint8_t>::iterator>
    nextMsg() {
      std::lock_guard<LL::SimpleLock> lg(lock);
      if (data.empty())
        return std::make_pair(data.end(), data.end());
      assert(data.size() >= 8);
      union { uint8_t a[4]; uint32_t b; } c;
      std::copy_n(data.begin(), 4, &c.a[0]);
      return std::make_pair(data.begin() + 4, data.begin() + 4 + c.b);
    }

    void popMsg() {
      std::lock_guard<LL::SimpleLock> lg(lock);
      assert(data.size() >= 8);
      union { uint8_t a[4]; uint32_t b; } c;
      std::copy_n(data.begin(), 4, &c.a[0]);
      data.erase(data.begin(), data.begin() + 4 + c.b);
    }

    //Worker thread interface
    void add(std::vector<uint8_t>& buf) {
      std::lock_guard<LL::SimpleLock> lg(lock);
      data.insert(data.end(), buf.begin(), buf.end());
    }
  };

  recvBuffer recvData;
  LL::SimpleLock recvLock;

  struct sendBuffer {
    std::vector<uint8_t> data;
    std::chrono::high_resolution_clock::time_point time;
    std::atomic<bool> urgent;
    LL::SimpleLock lock;

    void markUrgent() {
      urgent = true;
    }

    void add(SendBuffer& b) {
      std::lock_guard<LL::SimpleLock> lg(lock);
      if (data.empty())
        time = std::chrono::high_resolution_clock::now();
      union { uint8_t a[4]; uint32_t b; } c;
      c.b = b.size();
      data.insert(data.end(), &c.a[0], &c.a[4]);
      data.insert(data.end(), (uint8_t*)b.linearData(), (uint8_t*)b.linearData() + b.size());
    }

    //Worker thread Interface
    bool ready() {
      std::lock_guard<LL::SimpleLock> lg(lock);
      if (data.empty())
        return false;
      if (urgent)
        return true;
      if (data.size() > COMM_MIN)
        return true;
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - time);
      if (elapsed.count() > COMM_DELAY)
        return true;
      return false;
    }

  };

  std::vector<sendBuffer> sendData;

  void isend(uint32_t dest, SendBuffer& buf) {
    statSendNum += 1;
    statSendBytes += buf.size();
    auto& sd = sendData[dest];
    sd.add(buf);
  }

  void workerThread() {
    std::tie(netio, ID, Num) = makeNetworkIOMPI();
    ready = 1;
    while (ready != 2) {};
    while (ready != 3) {
      do {
        std::vector<uint8_t> rdata = netio->dequeue();
        if (rdata.empty())
          break;
        else
          recvData.add(rdata);
      } while (true);
      for(int i = 0; i < sendData.size(); ++i) {
        auto& sd = sendData[i];
        if (sd.ready()) {
          std::lock_guard<LL::SimpleLock> lg(sd.lock);
          netio->enqueue(i, sd.data);
          assert(sd.data.empty());
          sd.urgent = false;
        }
      }
    }
  }

  std::thread worker;
  std::atomic<int> ready;

public:
  using NetworkInterface::ID;
  using NetworkInterface::Num;

  NetworkInterfaceBuffered() {
    ready = 0;
    worker = std::thread(&NetworkInterfaceBuffered::workerThread, this);
    while (ready != 1) {};
    decltype(sendData) v(Num);
    sendData.swap(v);
    ready = 2; 
  }

  virtual ~NetworkInterfaceBuffered() {
    ready = 3;
    worker.join();
    delete netio;
  }

  virtual void send(uint32_t dest, recvFuncTy recv, SendBuffer& buf) {
    assert(recv);
    assert(dest < Num);
    buf.serialize_header((void*)recv);
    isend(dest, buf);
  }

  virtual void flush() {
    for (auto& sd : sendData)
      sd.markUrgent();
  }

  virtual bool handleReceives() {
    bool retval = false;
    if (recvLock.try_lock()) {
      std::lock_guard<LL::SimpleLock> lg(recvLock, std::adopt_lock);
      auto p = recvData.nextMsg();
      if (p.first != p.second) {
        retval = true;
        DeSerializeBuffer buf(p.first, p.second);
        statRecvNum += 1;
        statRecvBytes += buf.size();
        recvData.popMsg();
        uintptr_t fp = 0;
        gDeserialize(buf, fp);
        assert(fp);
        recvFuncTy f = (recvFuncTy)fp;
        f(buf);
      }
    }
    return retval;
  }
};

} //namespace ""

NetworkInterface& Galois::Runtime::makeNetworkBuffered() {
  static NetworkInterfaceBuffered net;
  return net;
}
