#include <iostream>
#include "Core/HW/GCMemcard/GCMemcard.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cout << "Please be a good user and give me a memcard path" << std::endl;
    return EXIT_FAILURE;
  }

//  std::cout << "About to attempt to load path " << argv[1] << " as a memory card..." << std::endl;
  auto [error, memcard] = Memcard::GCMemcard::Open(argv[1]);
  if (memcard) {
    //std::cout << static_cast<u32>(memcard->GetNumFiles()) << std::endl;
    for (auto i=0;i < memcard->GetNumFiles(); ++i)
    {
      auto index = memcard->GetFileIndex(i);
      auto saveMetadata = memcard->GetSaveComments(index);
      if (saveMetadata)
      {
  //      std::cout << "Content for file: " << static_cast<u32>(i) << saveMetadata->first << '\n' << saveMetadata->second << std::endl;
      }
    }
    auto data(memcard->GetSaveDataBytes(memcard->GetFileIndex(0)));
    if (data)
    {
      int j=0, lineCount = 0;
      for (int i=0; i<data->size();++i)
      {
        if (j == 0)
        {
          std::cout << lineCount << ": ";
        }
        printf("%02x ", (*data)[i]);
        if (j++ == 31)
        {
          std::cout << '\n';
          j = 0;
          lineCount++;
        }
      }
    }
    //std::cout << "memcard successfully loaded!" << std::endl;
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
