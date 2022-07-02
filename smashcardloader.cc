/*----- System Includes -----*/

#include <cstdio>
#include <random>
#include <limits>
#include <cstdint>
#include <utility>
#include <fstream>
#include <iostream>
#include <type_traits>
#include <unordered_set>

/*----- Local Includes -----*/

#include "argh.h"
#include "fmt/include/fmt/core.h"
#include "Core/HW/GCMemcard/GCMemcard.h"

/*----- Types -----*/

using namespace std::string_literals;

using Memcard::Savefile;
using Memcard::GCMemcard;
using Memcard::GCMemcardErrorCode;
using Memcard::GCMemcardImportFileRetVal;
using region_map = std::vector<std::vector<std::pair<std::size_t, std::size_t>>>;

struct extract_failed : std::runtime_error {
  extract_failed(std::string const& msg) : std::runtime_error(msg) {}
};

struct save_failed : std::runtime_error {
  save_failed(std::string const& msg) : std::runtime_error(msg) {}
};

template <class T, class U>
using forward_like_t = std::conditional_t<
  std::is_lvalue_reference_v<T>,
  std::remove_reference_t<U>&,
  std::remove_reference_t<U>&&
>;

/*----- Helpers -----*/

// Monkey patch something I think should exist to start with
namespace fmt {
  template <class Ptr, class Str, class... Args>
  void println(Ptr* stream, Str const& str, Args&&... args) {
    auto newline = str + "\n"s;
    print(stream, newline, std::forward<Args>(args)...);
  }

  template <class Str, class... Args>
  void println(Str const& str, Args&&... args) {
    println(stdout, str, std::forward<Args>(args)...);
  }
}

namespace {

template <class F, class... Args>
bool any_of(F&& func, Args&&... args) {
  return (std::forward<F>(func)(std::forward<Args>(args)) || ...);
}

template <class F, class... Args>
bool none_of(F&& func, Args&&... args) {
  return !any_of(std::forward<F>(func), std::forward<Args>(args)...);
}

template <class F, class... Args>
bool all_of(F&& func, Args&&... args) {
  return (std::forward<F>(func)(std::forward<Args>(args)) && ...);
}

template <class F, class... Args>
bool some_of(F&& func, Args&&... args) {
  return !all_of(std::forward<F>(func), std::forward<Args>(args)...);
}

template <class T, class U>
forward_like_t<T, U> forward_like(U&& val) noexcept {
  return static_cast<forward_like_t<T, U>>(val);
}

template <class F, class C, class... Cs>
void for_all(F&& func, C&& container, Cs&&... cs) {
  // Make sure everything is the same size.
  assert(all_of([] (auto& cont) { cont.size() == container.size(); }, cs...));

  // Collect and explode into iterators
  std::size_t count = 0;
  std::tuple iterators {container.begin(), cs.begin()...};
  while (std::get<0>(iterators) != container.end()) {
    std::apply([&func, &count] (auto& it, auto&... its) {
      // Compute whether our callable would like an explicit count
      constexpr bool takes_count = std::is_invocable_v<
        F,
        std::size_t,
        forward_like_t<C, decltype(*it)>,
        forward_like_t<Cs, decltype(*its)>...
      >;

      // Deference everything and call
      if constexpr (takes_count) {
        std::forward<F>(func)(count, forward_like<C>(*it), forward_like<Cs>(*its)...);
      } else {
        std::forward<F>(func)(forward_like<C>(*it), forward_like<Cs>(*its)...);
      }

      // Increment
      ++count;
      ++it, (++its, ...);
    }, iterators);
  }
}

/*----- Application Logic -----*/

void report_error(std::string_view name, GCMemcardErrorCode error) {
  if (error.Test(Memcard::GCMemcardValidityIssues::FAILED_TO_OPEN)) {
    fmt::println(stderr, R"(Failed to open card "{}")", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::IO_ERROR)) {
    fmt::println(stderr, R"(Detected an IO error while reading card "{})", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::INVALID_CARD_SIZE)) {
    fmt::println(stderr, R"(Detected an invalid card size while reading card "{}")", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::INVALID_CHECKSUM)) {
    fmt::println(stderr, R"(Detected an invalid checksum while reading card "{}")", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::MISMATCHED_CARD_SIZE)) {
    fmt::println(stderr, R"(Detected a card size mismatch while reading card "{}")", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::FREE_BLOCK_MISMATCH)) {
    fmt::println(stderr, R"(Detected a mismatch on free block count while reading card "{}")", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::DIR_BAT_INCONSISTENT)) {
    fmt::println(stderr, R"(Detected inconsistent backup data while reading card "{}")", name);
  } else if (error.Test(Memcard::GCMemcardValidityIssues::DATA_IN_UNUSED_AREA)) {
    fmt::println(stderr, R"(Detected unexpected data in an unused area while reading card "{}")", name);
  } else {
    fmt::println(R"(Detected an unknown error while reading card "{}")", name);
  }
  std::abort();
}

auto extract_save(GCMemcard const& card) {
  if (card.GetNumFiles() != 1) {
    throw std::runtime_error("smashcardloader currently only supports cards with a single save file");
  }

  auto save = card.ExportFile(0);
  if (!save) {
    throw extract_failed("Failed to extract save file");
  }
  return *save;
}

void store_save(GCMemcard& card, Savefile const& save) {
  if (card.GetNumFiles() != 1) {
    throw std::runtime_error("smashcardloader currently only supports cards with a single save file");
  }

  card.RemoveFile(0);
  auto res = card.ImportFile(save);
  if (res != GCMemcardImportFileRetVal::SUCCESS) {
    throw save_failed("Failed to overwrite original save data");
  }
  card.FixChecksums();
}

auto extract_filename(Savefile const& save) {
  auto& dir = save.dir_entry;
  auto* base = reinterpret_cast<char const*>(dir.m_filename.data());
  std::string name(base, base + dir.m_filename.size());
  return name;
}

auto calculate_diffs(Savefile const& lhscard, Savefile const& rhscard) {
  // Iterate over the blocks of both and diff
  region_map diffs;
  for_all([&diffs] (auto& lhsblock, auto& rhsblock) {
    // Setup bookeeping
    diffs.emplace_back();
    auto& lhsdata = lhsblock.m_block;
    auto& rhsdata = rhsblock.m_block;
    auto lhsptr = lhsdata.begin(), rhsptr = rhsdata.begin();
    assert(lhsdata.size() == rhsdata.size());

    // Iterate until we've exhausted all data.
    while (lhsptr != lhsdata.end()) {
      // Find the next disagreement.
      std::tie(lhsptr, rhsptr) = std::mismatch(lhsptr, lhsdata.end(), rhsptr);

      // Spin until we hit the end
      auto start = lhsptr;
      while (lhsptr != lhsdata.end() && *lhsptr != *rhsptr) ++lhsptr, ++rhsptr;

      // Compute offsets
      auto base = start - lhsdata.begin();
      auto diff = lhsptr - start;
      diffs.back().emplace_back(base, base + diff);
    }
  }, lhscard.blocks, rhscard.blocks);
  return diffs;
}

void print_diffs(region_map const& diffs) {
  int current = 0;
  for (auto& regions : diffs) {
    fmt::println("Printing diff ranges for block {}:", current++);
    std::string region = "[";
    for (auto& [start, end] : regions) {
      region += fmt::format(" [{}, {}],", start, end);
    }
    region.pop_back();
    region += " ]";
    fmt::println(region);
  }
}

void scramble_diffs(Savefile& card, region_map const& diffs,
    std::unordered_set<int> const& targets, int mutations, int chunk_size, int minimum_size) {
  // Initialize random engine once.
  std::mt19937 engine(std::random_device {}());
  std::uniform_int_distribution<std::uint8_t> rand_byte(0, 255);

  // Mutate the blocks
  int mutation_count = 0;
  for_all([&] (auto iteration, auto& block, auto& regions) {
    // Skip if given targets
    if (mutation_count >= mutations || !targets.empty() && !targets.count(iteration)) {
      return;
    }

    fmt::println("Will corrupt block {}...", iteration);
    auto& data = block.m_block;
    for (auto& [start, end] : regions) {
      if (end - start < minimum_size) {
        continue;
      }

      std::uniform_int_distribution<int> rand_offset(start, end);
      if (mutation_count < mutations) {
        fmt::println("Executing a corruption on block {}, between {}-{}...", iteration, start, end);
        auto base = data.begin() + rand_offset(engine);
        auto finish = base + chunk_size > data.end() ? data.end() : base + chunk_size;
        std::generate(base, finish, [&] { return rand_byte(engine); });
        ++mutation_count;
      } else {
        fmt::println("Reached maximum number of corruptions, {}, skipping the rest...", mutations);
        break;
      }
    }
  }, card.blocks, diffs);
}

}

/*----- Main -----*/

int main(int argc, char** argv) {
  // Setup CLI parser
  argh::parser cli;
  cli.add_param("scramble");
  cli.add_param("mutations");
  cli.add_param("chunk-size");
  cli.add_param("minimum-size");
  cli.parse(argc, argv);

  std::string lhs, rhs, output;
  if (any_of([] (auto&& arg) { return !arg; }, cli(1), cli(2))) {
    fmt::print(stderr, "You must supply at least two files to diff, and an optional one to output to");
    std::abort();
  }

  // Read our data
  cli(1) >> lhs;
  cli(2) >> rhs;
  cli(3, "/dev/null") >> output;
  fmt::println(R"(Diffing files "{}" and {}")", lhs, rhs);
  auto [lhserror, lhscard] = Memcard::GCMemcard::Open(lhs);
  auto [rhserror, rhscard] = Memcard::GCMemcard::Open(rhs);

  // Validate
  std::vector data {
    std::tie(lhs, lhscard, lhserror),
    std::tie(rhs, rhscard, rhserror)
  };
  for (auto& [name, card, error] : data) {
    if (!card) report_error(name, error);
  }

  // Extract both saves.
  auto lhssave = extract_save(*lhscard);
  auto rhssave = extract_save(*rhscard);
  auto lhsname = extract_filename(lhssave);
  auto rhsname = extract_filename(rhssave);
  fmt::println(R"(Name of first save is "{}" and name of second save is "{}")", lhsname, rhsname);

  // Compute regions
  fmt::println("Enumerating regions with diffs...");
  auto diffs = calculate_diffs(lhssave, rhssave);

  // Print diffs
  if (cli["print"]) {
    print_diffs(diffs);
  }

  // Collect corruption targets
  std::unordered_set<int> targets;
  int mutations, chunk_size, minimum_size;
  for (auto& param : cli.params("scramble")) {
    targets.insert(std::stoi(param.second));
  }
  cli("mutations", 1) >> mutations;
  cli("chunk-size", 1) >> chunk_size;
  cli("minimum-size", 1) >> minimum_size;

  // Corrupt regions randomly
  fmt::println("Corrupting regions with diffs...");
  scramble_diffs(lhssave, diffs, targets, mutations, chunk_size, minimum_size);

  // Update and exit
  fmt::println("Updating save file and writing to disk...");
  store_save(*lhscard, lhssave);
  lhscard->Save(output);
}
