#include "../board/bounded_queue.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    voiceedge::BoundedQueue<int> queue(2);
    require(queue.try_push(1), "first push should succeed");
    require(queue.try_push(2), "second push should succeed");
    require(!queue.try_push(3), "try_push must reject a full queue");

    int value = 0;
    require(queue.pop_for(value, std::chrono::milliseconds(10)), "first pop should succeed");
    require(value == 1, "FIFO order was not preserved");

    voiceedge::BoundedQueue<int> dropping_queue(2);
    dropping_queue.try_push(1);
    dropping_queue.try_push(2);
    require(dropping_queue.push_drop_oldest(3), "full monitor queue should drop its oldest item");
    require(dropping_queue.dropped_items() == 1, "drop counter should increment");
    require(dropping_queue.pop(value), "dropped queue first pop should succeed");
    require(value == 2, "drop-oldest policy kept the wrong item");
    require(dropping_queue.pop(value), "dropped queue second pop should succeed");
    require(value == 3, "newest item should be retained");

    dropping_queue.close();
    require(!dropping_queue.try_push(4), "closed queue must reject pushes");
    require(!dropping_queue.pop_for(value, std::chrono::milliseconds(1)),
            "closed and empty queue should not produce an item");

    std::cout << "bounded_queue_test_passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "bounded_queue_test_failed " << error.what() << '\n';
    return 1;
  }
}
