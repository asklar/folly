/*
 * Copyright 2015 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <folly/futures/Future.h>
#include <folly/futures/InlineExecutor.h>
#include <folly/futures/ManualExecutor.h>
#include <folly/futures/DrivableExecutor.h>
#include <folly/Baton.h>
#include <folly/MPMCQueue.h>

#include <thread>

using namespace folly;

struct ManualWaiter : public DrivableExecutor {
  explicit ManualWaiter(std::shared_ptr<ManualExecutor> ex) : ex(ex) {}

  void add(Func f) override {
    ex->add(f);
  }

  void drive() override {
    ex->wait();
    ex->run();
  }

  std::shared_ptr<ManualExecutor> ex;
};

struct ViaFixture : public testing::Test {
  ViaFixture() :
    westExecutor(new ManualExecutor),
    eastExecutor(new ManualExecutor),
    waiter(new ManualWaiter(westExecutor)),
    done(false)
  {
    t = std::thread([=] {
        ManualWaiter eastWaiter(eastExecutor);
        while (!done)
          eastWaiter.drive();
      });
  }

  ~ViaFixture() {
    done = true;
    eastExecutor->add([=]() { });
    t.join();
  }

  void addAsync(int a, int b, std::function<void(int&&)>&& cob) {
    eastExecutor->add([=]() {
      cob(a + b);
    });
  }

  std::shared_ptr<ManualExecutor> westExecutor;
  std::shared_ptr<ManualExecutor> eastExecutor;
  std::shared_ptr<ManualWaiter> waiter;
  InlineExecutor inlineExecutor;
  bool done;
  std::thread t;
};

TEST(Via, exceptionOnLaunch) {
  auto future = makeFuture<int>(std::runtime_error("E"));
  EXPECT_THROW(future.value(), std::runtime_error);
}

TEST(Via, thenValue) {
  auto future = makeFuture(std::move(1))
    .then([](Try<int>&& t) {
      return t.value() == 1;
    })
    ;

  EXPECT_TRUE(future.value());
}

TEST(Via, thenFuture) {
  auto future = makeFuture(1)
    .then([](Try<int>&& t) {
      return makeFuture(t.value() == 1);
    });
  EXPECT_TRUE(future.value());
}

static Future<std::string> doWorkStatic(Try<std::string>&& t) {
  return makeFuture(t.value() + ";static");
}

TEST(Via, thenFunction) {
  struct Worker {
    Future<std::string> doWork(Try<std::string>&& t) {
      return makeFuture(t.value() + ";class");
    }
    static Future<std::string> doWorkStatic(Try<std::string>&& t) {
      return makeFuture(t.value() + ";class-static");
    }
  } w;

  auto f = makeFuture(std::string("start"))
    .then(doWorkStatic)
    .then(Worker::doWorkStatic)
    .then(&Worker::doWork, &w)
    ;

  EXPECT_EQ(f.value(), "start;static;class-static;class");
}

TEST_F(ViaFixture, threadHops) {
  auto westThreadId = std::this_thread::get_id();
  auto f = via(eastExecutor.get()).then([=](Try<void>&& t) {
    EXPECT_NE(std::this_thread::get_id(), westThreadId);
    return makeFuture<int>(1);
  }).via(westExecutor.get()
  ).then([=](Try<int>&& t) {
    EXPECT_EQ(std::this_thread::get_id(), westThreadId);
    return t.value();
  });
  EXPECT_EQ(f.getVia(waiter.get()), 1);
}

TEST_F(ViaFixture, chainVias) {
  auto westThreadId = std::this_thread::get_id();
  auto f = via(eastExecutor.get()).then([=]() {
    EXPECT_NE(std::this_thread::get_id(), westThreadId);
    return 1;
  }).then([=](int val) {
    return makeFuture(val).via(westExecutor.get())
      .then([=](int val) mutable {
        EXPECT_EQ(std::this_thread::get_id(), westThreadId);
        return val + 1;
      });
  }).then([=](int val) {
    // even though ultimately the future that triggers this one executed in
    // the west thread, this then() inherited the executor from its
    // predecessor, ie the eastExecutor.
    EXPECT_NE(std::this_thread::get_id(), westThreadId);
    return val + 1;
  }).via(westExecutor.get()).then([=](int val) {
    // go back to west, so we can wait on it
    EXPECT_EQ(std::this_thread::get_id(), westThreadId);
    return val + 1;
  });

  EXPECT_EQ(f.getVia(waiter.get()), 4);
}

TEST_F(ViaFixture, bareViaAssignment) {
  auto f = via(eastExecutor.get());
}
TEST_F(ViaFixture, viaAssignment) {
  // via()&&
  auto f = makeFuture().via(eastExecutor.get());
  // via()&
  auto f2 = f.via(eastExecutor.get());
}

TEST(Via, chain1) {
  EXPECT_EQ(42,
            makeFuture()
            .thenMulti([] { return 42; })
            .get());
}

TEST(Via, chain3) {
  int count = 0;
  auto f = makeFuture().thenMulti(
      [&]{ count++; return 3.14159; },
      [&](double) { count++; return std::string("hello"); },
      [&]{ count++; return makeFuture(42); });
  EXPECT_EQ(42, f.get());
  EXPECT_EQ(3, count);
}

struct PriorityExecutor : public Executor {
  void add(Func f) override {}

  void addWithPriority(Func, int8_t priority) override {
    int mid = getNumPriorities() / 2;
    int p = priority < 0 ?
            std::max(0, mid + priority) :
            std::min(getNumPriorities() - 1, mid + priority);
    EXPECT_LT(p, 3);
    EXPECT_GE(p, 0);
    if (p == 0) {
      count0++;
    } else if (p == 1) {
      count1++;
    } else if (p == 2) {
      count2++;
    }
  }

  uint8_t getNumPriorities() const override {
    return 3;
  }

  int count0{0};
  int count1{0};
  int count2{0};
};

TEST(Via, priority) {
  PriorityExecutor exe;
  via(&exe, -1).then([]{});
  via(&exe, 0).then([]{});
  via(&exe, 1).then([]{});
  via(&exe, 42).then([]{});  // overflow should go to max priority
  via(&exe, -42).then([]{}); // underflow should go to min priority
  via(&exe).then([]{});      // default to mid priority
  via(&exe, Executor::LO_PRI).then([]{});
  via(&exe, Executor::HI_PRI).then([]{});
  EXPECT_EQ(3, exe.count0);
  EXPECT_EQ(2, exe.count1);
  EXPECT_EQ(3, exe.count2);
}

TEST_F(ViaFixture, chainX1) {
  EXPECT_EQ(42,
            makeFuture()
            .thenMultiWithExecutor(eastExecutor.get(),[] { return 42; })
            .get());
}

TEST_F(ViaFixture, chainX3) {
  auto westThreadId = std::this_thread::get_id();
  int count = 0;
  auto f = via(westExecutor.get()).thenMultiWithExecutor(
      eastExecutor.get(),
      [&]{
        EXPECT_NE(std::this_thread::get_id(), westThreadId);
        count++; return 3.14159;
      },
      [&](double) { count++; return std::string("hello"); },
      [&]{ count++; })
    .then([&](){
        EXPECT_EQ(std::this_thread::get_id(), westThreadId);
        return makeFuture(42);
    });
  EXPECT_EQ(42, f.getVia(waiter.get()));
  EXPECT_EQ(3, count);
}

TEST(Via, then2) {
  ManualExecutor x1, x2;
  bool a = false, b = false, c = false;
  via(&x1)
    .then([&]{ a = true; })
    .then(&x2, [&]{ b = true; })
    .then([&]{ c = true; });

  EXPECT_FALSE(a);
  EXPECT_FALSE(b);

  x1.run();
  EXPECT_TRUE(a);
  EXPECT_FALSE(b);
  EXPECT_FALSE(c);

  x2.run();
  EXPECT_TRUE(b);
  EXPECT_FALSE(c);

  x1.run();
  EXPECT_TRUE(c);
}

TEST(Via, then2Variadic) {
  struct Foo { bool a = false; void foo(Try<void>) { a = true; } };
  Foo f;
  ManualExecutor x;
  makeFuture().then(&x, &Foo::foo, &f);
  EXPECT_FALSE(f.a);
  x.run();
  EXPECT_TRUE(f.a);
}

/// Simple executor that does work in another thread
class ThreadExecutor : public Executor {
  folly::MPMCQueue<Func> funcs;
  std::atomic<bool> done {false};
  std::thread worker;
  folly::Baton<> baton;

  void work() {
    baton.post();
    Func fn;
    while (!done) {
      while (!funcs.isEmpty()) {
        funcs.blockingRead(fn);
        fn();
      }
    }
  }

 public:
  explicit ThreadExecutor(size_t n = 1024)
    : funcs(n) {
    worker = std::thread(std::bind(&ThreadExecutor::work, this));
  }

  ~ThreadExecutor() {
    done = true;
    funcs.write([]{});
    worker.join();
  }

  void add(Func fn) override {
    funcs.blockingWrite(std::move(fn));
  }

  void waitForStartup() {
    baton.wait();
  }
};

TEST(Via, viaThenGetWasRacy) {
  ThreadExecutor x;
  std::unique_ptr<int> val = folly::via(&x)
    .then([] { return folly::make_unique<int>(42); })
    .get();
  ASSERT_TRUE(!!val);
  EXPECT_EQ(42, *val);
}

class DummyDrivableExecutor : public DrivableExecutor {
 public:
  void add(Func f) override {}
  void drive() override { ran = true; }
  bool ran{false};
};

TEST(Via, getVia) {
  {
    // non-void
    ManualExecutor x;
    auto f = via(&x).then([]{ return true; });
    EXPECT_TRUE(f.getVia(&x));
  }

  {
    // void
    ManualExecutor x;
    auto f = via(&x).then();
    f.getVia(&x);
  }

  {
    DummyDrivableExecutor x;
    auto f = makeFuture(true);
    EXPECT_TRUE(f.getVia(&x));
    EXPECT_FALSE(x.ran);
  }
}

TEST(Via, waitVia) {
  {
    ManualExecutor x;
    auto f = via(&x).then();
    EXPECT_FALSE(f.isReady());
    f.waitVia(&x);
    EXPECT_TRUE(f.isReady());
  }

  {
    // try rvalue as well
    ManualExecutor x;
    auto f = via(&x).then().waitVia(&x);
    EXPECT_TRUE(f.isReady());
  }

  {
    DummyDrivableExecutor x;
    makeFuture(true).waitVia(&x);
    EXPECT_FALSE(x.ran);
  }
}

TEST(Via, viaRaces) {
  ManualExecutor x;
  Promise<void> p;
  auto tid = std::this_thread::get_id();
  bool done = false;

  std::thread t1([&] {
    p.getFuture()
      .via(&x)
      .then([&](Try<void>&&) { EXPECT_EQ(tid, std::this_thread::get_id()); })
      .then([&](Try<void>&&) { EXPECT_EQ(tid, std::this_thread::get_id()); })
      .then([&](Try<void>&&) { done = true; });
  });

  std::thread t2([&] {
    p.setValue();
  });

  while (!done) x.run();
  t1.join();
  t2.join();
}
