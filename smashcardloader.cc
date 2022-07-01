#include <iostream>
#include "Core/HW/GCMemcard/GCMemcard.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cout << "Please be a good user and give me a memcard path" << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "About to attempt to load path " << argv[1] << " as a memory card..." << std::endl;
  auto [error, memcard] = Memcard::GCMemcard::Open(argv[1]);
  if (memcard) {
    std::cout << "memcard successfully loaded!" << std::endl;
  } else {
    std::cout << "memcard loading failed..." << std::endl;

    if (error.Test(Memcard::GCMemcardValidityIssues::FAILED_TO_OPEN)) {
      std::cout << "Failed to open the card" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::IO_ERROR)) {
      std::cout << "Encountered an IO error" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::INVALID_CARD_SIZE)) {
      std::cout << "Invalid card size detected" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::INVALID_CHECKSUM)) {
      std::cout << "Invalid checksum detected" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::MISMATCHED_CARD_SIZE)) {
      std::cout << "Mismatched card size detected" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::FREE_BLOCK_MISMATCH)) {
      std::cout << "Free block mismatch detected" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::DIR_BAT_INCONSISTENT)) {
      std::cout << "Dir bat is inconsistent" << std::endl;
    } else if (error.Test(Memcard::GCMemcardValidityIssues::DATA_IN_UNUSED_AREA)) {
      std::cout << "Detected data in an unused area" << std::endl;
    } else {
      std::cout << "Detected error is unknown!" << std::endl;
    }
  }
}
