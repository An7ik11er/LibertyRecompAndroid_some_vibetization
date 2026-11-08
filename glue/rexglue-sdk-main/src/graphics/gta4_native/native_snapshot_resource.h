#pragma once
// NativeSnapshotResource — пул снапшотов пайплайна/шейдерного состояния.
//
// Контракт (как в оригинале): allocate() зовёт ТОЛЬКО один поток (render worker),
// deallocate() — любой поток (ретайреры, рекордер, гость).
//
// Отличие от патча с thread_local TlsReturnBatch:
//   * никаких thread_local-указателей на пул -> нет use-after-free, когда пул
//     уничтожен/пересоздан, а у какого-то потока в TLS ещё лежит batch с
//     owner == старый this (тот же адрес у нового пула = чужие указатели в free-list);
//   * возврат — lock-free push в интрузивный стек (CAS), забор — один exchange()
//     всего списка. "Pop-all" через exchange не подвержен ABA.
//   * мьютекс остался только для роста (новый блок / новый класс размера) —
//     это единицы раз за игру.
//
// 29.10: раскладка по кэш-линиям. Раньше SizeClass {size, local, returned} лежали
// подряд по 24 байта: аллокатор (GtaRenderWork, Big) писал local в ту же 64-байтную
// линию, в которую ретайрер (GtaCmdRetirer, Little) делал CAS по returned, — линия
// ходила между ядрами на каждый снапшот (тысячи раз за кадр в обе стороны).
// Теперь: размеры классов и счётчик — линия только для чтения; local[] — линия
// только аллокатора; у каждого returned своя линия. Поведение то же.
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <mutex>
#include <new>
#include <vector>

namespace rex::graphics::gta4_native {

class NativeSnapshotResource final : public std::pmr::memory_resource {
 public:
  NativeSnapshotResource() = default;
  NativeSnapshotResource(const NativeSnapshotResource&) = delete;
  NativeSnapshotResource& operator=(const NativeSnapshotResource&) = delete;
  ~NativeSnapshotResource() override {
    for (void* block : blocks_) ::operator delete(block, std::align_val_t(kAlignment));
  }

 private:
  static constexpr size_t kAlignment = 16;
  static constexpr size_t kChunksPerBlock = 64;
  static constexpr size_t kMaximumClasses = 8;
  static constexpr size_t kCacheLine = 64;  // A55/A78/X1, а также DSU L3

  struct FreeNode {
    FreeNode* next;
  };
  static_assert(sizeof(FreeNode) <= kAlignment);

  // Одна линия на класс: сюда пишут только освобождающие потоки (CAS) и
  // аллокатор при заборе всего списка (exchange, раз в десятки аллокаций).
  struct alignas(kCacheLine) Returned {
    std::atomic<FreeNode*> head{nullptr};
  };
  static_assert(sizeof(Returned) == kCacheLine);

  static size_t Rounded(size_t bytes) { return (bytes + kAlignment - 1) & ~(kAlignment - 1); }

  // Индекс класса или kMaximumClasses.
  size_t Find(size_t size) const {
    const size_t count = class_count_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
      if (sizes_[i] == size) return i;
    return kMaximumClasses;
  }

  void* do_allocate(size_t bytes, size_t alignment) override {
    if (alignment > kAlignment || !bytes) return ::operator new(bytes, std::align_val_t(alignment));
    const size_t size = Rounded(bytes);
    size_t index = Find(size);
    if (index == kMaximumClasses) {
      std::lock_guard lock(grow_mutex_);
      const size_t count = class_count_.load(std::memory_order_relaxed);
      if (count == kMaximumClasses) return ::operator new(bytes, std::align_val_t(alignment));
      index = count;
      sizes_[index] = size;
      class_count_.store(count + 1, std::memory_order_release);  // публикуем size
    }
    FreeNode*& local = local_[index];
    if (!local) {
      // Забрать всё, что вернули другие потоки, одним атомарным обменом.
      local = returned_[index].head.exchange(nullptr, std::memory_order_acquire);
      if (!local) {
        auto* block = static_cast<std::byte*>(
            ::operator new(size * kChunksPerBlock, std::align_val_t(kAlignment)));
        {
          std::lock_guard lock(grow_mutex_);
          blocks_.push_back(block);
        }
        FreeNode* head = nullptr;
        for (size_t i = kChunksPerBlock; i-- > 0;) {
          auto* node = reinterpret_cast<FreeNode*>(block + i * size);
          node->next = head;
          head = node;
        }
        local = head;
      }
    }
    FreeNode* node = local;
    local = node->next;
    // 31.10: the next free block was last written by the freeing thread
    // (GtaCmdRetirer, a Little core): taking it costs a cache-line transfer
    // (do_allocate was 10.7% of GtaRenderWork). Start that transfer now, for
    // writing, so it overlaps the work until the next allocation.
    if (local) __builtin_prefetch(local, 1, 3);
    return node;
  }

  void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
    if (alignment > kAlignment || !bytes) {
      ::operator delete(pointer, std::align_val_t(alignment));
      return;
    }
    const size_t index = Find(Rounded(bytes));
    if (index == kMaximumClasses) {  // выделено, когда таблица классов была полна
      ::operator delete(pointer, std::align_val_t(alignment));
      return;
    }
    auto* node = static_cast<FreeNode*>(pointer);
    std::atomic<FreeNode*>& returned = returned_[index].head;
    FreeNode* head = returned.load(std::memory_order_relaxed);
    do {
      node->next = head;
    } while (!returned.compare_exchange_weak(head, node, std::memory_order_release,
                                             std::memory_order_relaxed));
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  // Читается всеми потоками на каждый вызов, пишется единицы раз за игру.
  alignas(kCacheLine) std::atomic<size_t> class_count_{0};
  size_t sizes_[kMaximumClasses]{};  // записывается до публикации class_count_
  // Только поток-аллокатор.
  alignas(kCacheLine) FreeNode* local_[kMaximumClasses]{};
  // push из любых потоков; у каждого класса своя линия.
  Returned returned_[kMaximumClasses];
  // Рост: под grow_mutex_.
  alignas(kCacheLine) std::mutex grow_mutex_;
  std::vector<void*> blocks_;
};

}  // namespace rex::graphics::gta4_native
