set(LLVM_LINK_COMPONENTS
  BitReader
  BitWriter
  NaClBitAnalysis
  NaClBitTestUtils
  NaClBitReader
  NaClBitWriter
  Core
  Support
  )

add_llvm_unittest(BitcodeTests
  BitReaderTest.cpp
  NaClAbbrevTrieTest.cpp
  NaClObjDumpTest.cpp
  NaClObjDumpTypesTest.cpp
  NaClParseTypesTest.cpp
  NaClParseInstsTest.cpp
  NaClTextFormatterTest.cpp
  )
