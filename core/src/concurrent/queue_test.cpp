#include <userver/concurrent/queue.hpp>

#include <optional>

#include <userver/engine/mutex.hpp>
#include <userver/utils/async.hpp>
#include "mp_queue_test.hpp"

#include <userver/utest/utest.hpp>

namespace {
constexpr std::size_t kProducersCount = 4;
constexpr std::size_t kConsumersCount = 4;
constexpr std::size_t kMessageCount = 1000;

template <typename Producer>
auto GetProducerTask(const Producer& producer, std::size_t i) {
  return utils::Async("producer", [&producer = producer, i] {
    for (std::size_t message = i * kMessageCount;
         message < (i + 1) * kMessageCount; ++message) {
      ASSERT_TRUE(producer.Push(std::size_t{message}));
    }
  });
}

using TestMpmcTypes =
    testing::Types<concurrent::NonFifoMpmcQueue<int>,
                   concurrent::NonFifoMpmcQueue<std::unique_ptr<int>>,
                   concurrent::NonFifoMpmcQueue<std::unique_ptr<RefCountData>>>;
using TestMpscTypes =
    testing::Types<concurrent::NonFifoMpscQueue<int>,
                   concurrent::NonFifoMpscQueue<std::unique_ptr<int>>,
                   concurrent::NonFifoMpscQueue<std::unique_ptr<RefCountData>>>;
}  // namespace

INSTANTIATE_TYPED_UTEST_SUITE_P(NonFifoMpmcQueue, QueueFixture,
                                concurrent::NonFifoMpmcQueue<int>);

INSTANTIATE_TYPED_UTEST_SUITE_P(NonFifoMpmcQueue, TypedQueueFixture,
                                ::TestMpmcTypes);

INSTANTIATE_TYPED_UTEST_SUITE_P(NonFifoMpscQueue, QueueFixture,
                                concurrent::NonFifoMpscQueue<int>);

INSTANTIATE_TYPED_UTEST_SUITE_P(NonFifoMpscQueue, TypedQueueFixture,
                                ::TestMpmcTypes);

UTEST(NonFifoMpmcQueue, ConsumerIsDead) {
  auto queue = concurrent::NonFifoMpmcQueue<int>::Create();
  auto producer = queue->GetProducer();

  (void)(queue->GetConsumer());
  EXPECT_FALSE(producer.Push(0));
}

UTEST_MT(NonFifoMpmcQueue, Mpmc, kProducersCount + kMessageCount) {
  auto queue = concurrent::NonFifoMpmcQueue<std::size_t>::Create(kMessageCount);
  std::vector<concurrent::NonFifoMpmcQueue<std::size_t>::Producer> producers;
  producers.reserve(kProducersCount);
  for (std::size_t i = 0; i < kProducersCount; ++i) {
    producers.emplace_back(queue->GetProducer());
  }

  std::vector<engine::TaskWithResult<void>> producers_tasks;
  producers_tasks.reserve(kProducersCount);
  for (std::size_t i = 0; i < kProducersCount; ++i) {
    producers_tasks.push_back(GetProducerTask(producers[i], i));
  }

  std::vector<concurrent::NonFifoMpmcQueue<std::size_t>::Consumer> consumers;
  consumers.reserve(kConsumersCount);
  for (std::size_t i = 0; i < kConsumersCount; ++i) {
    consumers.emplace_back(queue->GetConsumer());
  }

  std::vector<int> consumed_messages(kMessageCount * kProducersCount, 0);
  engine::Mutex mutex;

  std::vector<engine::TaskWithResult<void>> consumers_tasks;
  consumers_tasks.reserve(kConsumersCount);
  for (std::size_t i = 0; i < kConsumersCount; ++i) {
    consumers_tasks.push_back(utils::Async(
        "consumer", [& consumer = consumers[i], &consumed_messages, &mutex] {
          std::size_t value{};
          while (consumer.Pop(value)) {
            std::lock_guard lock(mutex);
            ++consumed_messages[value];
          }
        }));
  }

  for (auto& task : producers_tasks) {
    task.Get();
  }
  producers.clear();

  for (auto& task : consumers_tasks) {
    task.Get();
  }

  ASSERT_TRUE(std::all_of(consumed_messages.begin(), consumed_messages.end(),
                          [](int item) { return item == 1; }));
}

UTEST_MT(NonFifoMpmcQueue, SizeAfterConsumersDie, 4) {
  constexpr std::size_t kConsumersCount = 3;
  constexpr std::size_t kAtemptsCount = 1000;

  auto queue = concurrent::NonFifoMpmcQueue<int>::Create();

  auto producer =
      std::make_optional<concurrent::NonFifoMpmcQueue<int>::Producer>(
          queue->GetProducer());

  EXPECT_EQ(queue->GetSizeApproximate(), 0);

  std::vector<engine::TaskWithResult<void>> tasks;
  tasks.reserve(kConsumersCount);
  for (std::size_t i = 0; i < kConsumersCount; ++i) {
    tasks.push_back(utils::Async("consumer", [consumer = queue->GetConsumer()] {
      for (std::size_t atempt = 0; atempt < kAtemptsCount; ++atempt) {
        int value{0};
        ASSERT_FALSE(consumer.Pop(value));
      }
    }));
  }

  // Kill the producer and try to provoke a race
  producer.reset();

  engine::Yield();
  engine::Yield();
  engine::Yield();

  for (std::size_t atempt = 0; atempt < kAtemptsCount / 10; ++atempt) {
    EXPECT_EQ(0, queue->GetSizeApproximate());
  }

  for (auto&& task : tasks) {
    task.Get();
  }
  EXPECT_EQ(0, queue->GetSizeApproximate());
}

UTEST_MT(NonFifoSpmcQueue, Spmc, 1 + kConsumersCount) {
  auto queue = concurrent::NonFifoSpmcQueue<std::size_t>::Create(kMessageCount);
  auto producer =
      std::make_optional<concurrent::NonFifoSpmcQueue<std::size_t>::Producer>(
          queue->GetProducer());

  auto producer_task = (GetProducerTask(*producer, 0));

  std::vector<concurrent::NonFifoSpmcQueue<std::size_t>::Consumer> consumers;
  consumers.reserve(kConsumersCount);
  for (std::size_t i = 0; i < kConsumersCount; ++i) {
    consumers.emplace_back(queue->GetConsumer());
  }

  std::vector<int> consumed_messages(kMessageCount * 1, 0);
  engine::Mutex mutex;

  std::vector<engine::TaskWithResult<void>> consumers_tasks;
  consumers_tasks.reserve(kConsumersCount);
  for (std::size_t i = 0; i < kConsumersCount; ++i) {
    consumers_tasks.push_back(utils::Async(
        "consumer", [& consumer = consumers[i], &consumed_messages, &mutex] {
          std::size_t value{};
          while (consumer.Pop(value)) {
            std::lock_guard lock(mutex);
            ++consumed_messages[value];
          }
        }));
  }

  producer_task.Get();
  producer.reset();

  for (auto& task : consumers_tasks) {
    task.Get();
  }

  ASSERT_TRUE(std::all_of(consumed_messages.begin(), consumed_messages.end(),
                          [](int item) { return item == 1; }));
}

UTEST_MT(NonFifoMpscQueue, Mpsc, kProducersCount + 1) {
  auto queue = concurrent::NonFifoMpscQueue<std::size_t>::Create(kMessageCount);
  std::vector<concurrent::NonFifoMpscQueue<std::size_t>::Producer> producers;
  auto consumer = queue->GetConsumer();

  producers.reserve(kProducersCount);
  for (std::size_t i = 0; i < kProducersCount; ++i) {
    producers.emplace_back(queue->GetProducer());
  }

  std::vector<engine::TaskWithResult<void>> producers_tasks;
  producers_tasks.reserve(kProducersCount);
  for (std::size_t i = 0; i < kProducersCount; ++i) {
    producers_tasks.push_back(GetProducerTask(producers[i], i));
  }

  std::vector<int> consumed_messages(kMessageCount * kProducersCount, 0);

  auto consumer_task = utils::Async("consumer", [&] {
    std::size_t value{};
    while (consumer.Pop(value)) {
      ++consumed_messages[value];
    }
  });

  for (auto& task : producers_tasks) {
    task.Get();
  }
  producers.clear();

  consumer_task.Get();

  ASSERT_TRUE(std::all_of(consumed_messages.begin(), consumed_messages.end(),
                          [](int item) { return item == 1; }));
}

UTEST_MT(NonFifoMpscQueue, SizeAfterConsumersDie, 2) {
  constexpr std::size_t kAtemptsCount = 1000;

  auto queue = concurrent::NonFifoMpmcQueue<int>::Create();

  auto producer =
      std::make_optional<concurrent::NonFifoMpmcQueue<int>::Producer>(
          queue->GetProducer());

  EXPECT_EQ(queue->GetSizeApproximate(), 0);

  auto task = utils::Async("consumer", [consumer = queue->GetConsumer()] {
    for (std::size_t atempt = 0; atempt < kAtemptsCount; ++atempt) {
      int value{0};
      ASSERT_FALSE(consumer.Pop(value));
    }
  });

  // Kill the producer and try to provoke a race
  producer.reset();

  engine::Yield();
  engine::Yield();
  engine::Yield();

  for (std::size_t atempt = 0; atempt < kAtemptsCount / 10; ++atempt) {
    EXPECT_EQ(0, queue->GetSizeApproximate());
  }

  task.Get();
  EXPECT_EQ(0, queue->GetSizeApproximate());
}

UTEST_MT(NonFifoSpscQueue, Spsc, 1 + 1) {
  auto queue = concurrent::NonFifoSpscQueue<std::size_t>::Create(kMessageCount);

  auto producer =
      std::make_optional<concurrent::NonFifoSpscQueue<std::size_t>::Producer>(
          queue->GetProducer());
  auto producer_task = GetProducerTask(*producer, 0);

  auto consumer = queue->GetConsumer();
  std::vector<int> consumed_messages(kMessageCount * 1, 0);
  auto consumer_task = utils::Async("consumer", [&] {
    std::size_t value{};
    while (consumer.Pop(value)) {
      ++consumed_messages[value];
    }
  });

  producer_task.Get();
  producer.reset();
  consumer_task.Get();

  ASSERT_TRUE(std::all_of(consumed_messages.begin(), consumed_messages.end(),
                          [](int item) { return item == 1; }));
}
