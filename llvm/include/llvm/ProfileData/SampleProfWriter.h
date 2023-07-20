//===- SampleProfWriter.h - Write LLVM sample profile data ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions needed for writing sample profiles.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_PROFILEDATA_SAMPLEPROFWRITER_H
#define LLVM_PROFILEDATA_SAMPLEPROFWRITER_H

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/ProfileSummary.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <memory>
#include <set>
#include <system_error>

namespace llvm {
namespace sampleprof {

enum SectionLayout {
  DefaultLayout,
  // The layout splits profile with context information from profile without
  // context information. When Thinlto is enabled, ThinLTO postlink phase only
  // has to load profile with context information and can skip the other part.
  CtxSplitLayout,
  NumOfLayout,
};


class DefaultFunctionPruningStrategy {
  std::vector<NameFunctionSamples> SortedFunctions;
  size_t OutputSizeLimit;
  size_t Index;
  size_t Step;

public:
  DefaultFunctionPruningStrategy(SampleProfileMap &ProfileMap,
                                 size_t OutputSizeLimit);

  operator const llvm::ArrayRef<NameFunctionSamples>() const {
    return ArrayRef<NameFunctionSamples>(SortedFunctions.data(), Index);
  }

  bool operator()(size_t CurrentOutputSize);
};

/// Sample-based profile writer. Base class.
class SampleProfileWriter {
public:
  using FunctionSamplesVector = const llvm::ArrayRef<NameFunctionSamples>;

  virtual ~SampleProfileWriter() = default;

  /// Write sample profiles in \p S.
  ///
  /// \returns status code of the file update operation.
  virtual std::error_code writeSample(const FunctionSamples &S) = 0;

  /// Write all the sample profiles in the given map of samples.
  ///
  /// \returns status code of the file update operation.
  std::error_code write(const SampleProfileMap &ProfileMap) {
    std::vector<NameFunctionSamples> SortedProfiles;
    sortFuncProfiles(ProfileMap, SortedProfiles);
    return write(SortedProfiles);
  }

  virtual std::error_code write(FunctionSamplesVector &ProfileMap);

  /// Write sample profiles up to a given size limit, using a strategy to
  /// iteratively reduce the amount of content being written.
  /// The default strategy uses binary search on the vector of function samples
  /// sorted by their total count, to find a threshold that only function
  /// samples having a total count greater than which are written.
  /// The strategy should be a class providing the following interfaces:
  /// (1) A constructor taking a SampleProfileMap and a size_t for output size
  /// limit. The profile map may be modified by this class.
  /// (2) A type conversion function returning a representation of the current
  /// sample profiles to be written.
  /// (3) bool operator()(size_t), called after each iteration to write the
  /// current sample profiles with the argument being the size written. This
  /// function should either modify the profile map or the class' internal state
  /// so that in the next iteration (2) returns a profile map (or a slice of it)
  /// with fewer contents and return true, or return false to indicate it cannot
  /// modify the profile map further.
  /// (2) and (3) are called iteratively until either the strategy terminates,
  /// or all function samples are removed. The strategy should ensure itself
  /// will terminate.
  /// Generally the strategy should sort the profile map and return an ArrayRef
  /// of the sorted NameFunctionSamples for (2), as sample profiles are always
  /// writen in sorted order, and by doing this it greatly improves performance
  /// by not sorting the sample profiles in every iteration. However this
  /// is not a mandatory requirement.
  ///
  /// \returns status code of the file update operation.
  template <typename Strategy = DefaultFunctionPruningStrategy>
  std::error_code writeWithSizeLimit(SampleProfileMap &ProfileMap,
                                     size_t OutputSizeLimit) {
    if (OutputSizeLimit == 0)
      return write(ProfileMap);

    Strategy S(ProfileMap, OutputSizeLimit);

    size_t TotalSize;
    std::unique_ptr<raw_ostream> OriginalOutputStream;
    OutputStream.swap(OriginalOutputStream);
    SmallVector<char> StringBuffer;
    do {
      // Write the current slice of the profile map to a memory buffer to check
      // the output size.
      StringBuffer.clear();
      OutputStream.reset(new raw_svector_ostream(StringBuffer));
      if (std::error_code EC = write(S))
        return EC;

      TotalSize = StringBuffer.size();
      // On Windows every "\n" is actually written as "\r\n" to disk but not to
      // memory buffer, this difference should be added when calculating the
      // total output size.
#ifdef _WIN32
      if (Format == SPF_Text)
        TotalSize += LineCount;
#endif

      // If no function can be written, exit with error to indicate the output
      // file is not generated, unless we started with an empty profile map.
      if (ProfileMap.empty())
        break;
      else if ((Format == SPF_Text && LineCount == 0) ||
               ((Format == SPF_Binary || Format == SPF_Ext_Binary)
                && Summary->getNumFunctions() == 0))
        return sampleprof_error::too_large;

      // Update the strategy with the output size from this iteration.
    } while (S(TotalSize));

    // If the strategy cannot reduce the sample profiles within the size limit,
    // signal an error.
    if (TotalSize > OutputSizeLimit)
      return sampleprof_error::too_large;

    // Write the reduced sample profile back to the actual output.
    OutputStream.swap(OriginalOutputStream);
    OutputStream->write(StringBuffer.data(), StringBuffer.size());
    return sampleprof_error::success;
  }

  raw_ostream &getOutputStream() { return *OutputStream; }

  /// Profile writer factory.
  ///
  /// Create a new file writer based on the value of \p Format.
  static ErrorOr<std::unique_ptr<SampleProfileWriter>>
  create(StringRef Filename, SampleProfileFormat Format);

  /// Create a new stream writer based on the value of \p Format.
  /// For testing.
  static ErrorOr<std::unique_ptr<SampleProfileWriter>>
  create(std::unique_ptr<raw_ostream> &OS, SampleProfileFormat Format);

  virtual void setProfileSymbolList(ProfileSymbolList *PSL) {}
  virtual void setToCompressAllSections() {}
  virtual void setUseMD5() {}
  virtual void setPartialProfile() {}
  virtual void resetSecLayout(SectionLayout SL) {}

protected:
  SampleProfileWriter(std::unique_ptr<raw_ostream> &OS)
      : OutputStream(std::move(OS)) {}

  /// Write a file header for the profile file.
  virtual std::error_code writeHeader(FunctionSamplesVector &ProfileMap) = 0;

  // Write function profiles to the profile file.
  virtual std::error_code writeFuncProfiles(FunctionSamplesVector &ProfileMap);

  /// For writeWithSizeLimit in text mode, each newline takes 1 additional byte
  /// on Windows when actually written to the file, but not written to a memory
  /// buffer. This needs to be accounted for when rewriting the profile.
  size_t LineCount;

  /// Output stream where to emit the profile to.
  std::unique_ptr<raw_ostream> OutputStream;

  /// Profile summary.
  std::unique_ptr<ProfileSummary> Summary;

  /// Compute summary for this profile.
  void computeSummary(FunctionSamplesVector &ProfileMap);

  /// Profile format.
  SampleProfileFormat Format = SPF_None;
};

/// Sample-based profile writer (text format).
class SampleProfileWriterText : public SampleProfileWriter {
public:
  std::error_code writeSample(const FunctionSamples &S) override;

protected:
  SampleProfileWriterText(std::unique_ptr<raw_ostream> &OS)
      : SampleProfileWriter(OS), Indent(0) {}

  std::error_code writeHeader(FunctionSamplesVector &ProfileMap) override {
    LineCount = 0;
    return sampleprof_error::success;
  }

private:
  /// Indent level to use when writing.
  ///
  /// This is used when printing inlined callees.
  unsigned Indent;

  friend ErrorOr<std::unique_ptr<SampleProfileWriter>>
  SampleProfileWriter::create(std::unique_ptr<raw_ostream> &OS,
                              SampleProfileFormat Format);
};

/// Sample-based profile writer (binary format).
class SampleProfileWriterBinary : public SampleProfileWriter {
public:
  SampleProfileWriterBinary(std::unique_ptr<raw_ostream> &OS)
      : SampleProfileWriter(OS) {}

  std::error_code writeSample(const FunctionSamples &S) override;

protected:
  virtual MapVector<StringRef, uint32_t> &getNameTable() { return NameTable; }
  virtual std::error_code writeMagicIdent(SampleProfileFormat Format);
  virtual std::error_code writeNameTable();
  std::error_code writeHeader(FunctionSamplesVector &ProfileMap) override;
  std::error_code writeSummary();
  virtual std::error_code writeContextIdx(const SampleContext &Context);
  std::error_code writeNameIdx(StringRef FName);
  std::error_code writeBody(const FunctionSamples &S);
  inline void stablizeNameTable(MapVector<StringRef, uint32_t> &NameTable,
                                std::set<StringRef> &V);

  MapVector<StringRef, uint32_t> NameTable;

  void addName(StringRef FName);
  virtual void addContext(const SampleContext &Context);
  void addNames(const FunctionSamples &S);

private:
  friend ErrorOr<std::unique_ptr<SampleProfileWriter>>
  SampleProfileWriter::create(std::unique_ptr<raw_ostream> &OS,
                              SampleProfileFormat Format);
};

class SampleProfileWriterRawBinary : public SampleProfileWriterBinary {
  using SampleProfileWriterBinary::SampleProfileWriterBinary;
};

const std::array<SmallVector<SecHdrTableEntry, 8>, NumOfLayout>
    ExtBinaryHdrLayoutTable = {
        // Note that SecFuncOffsetTable section is written after SecLBRProfile
        // in the profile, but is put before SecLBRProfile in SectionHdrLayout.
        // This is because sample reader follows the order in SectionHdrLayout
        // to read each section. To read function profiles on demand, sample
        // reader need to get the offset of each function profile first.
        //
        // DefaultLayout
        SmallVector<SecHdrTableEntry, 8>({{SecProfSummary, 0, 0, 0, 0},
                                          {SecNameTable, 0, 0, 0, 0},
                                          {SecCSNameTable, 0, 0, 0, 0},
                                          {SecFuncOffsetTable, 0, 0, 0, 0},
                                          {SecLBRProfile, 0, 0, 0, 0},
                                          {SecProfileSymbolList, 0, 0, 0, 0},
                                          {SecFuncMetadata, 0, 0, 0, 0}}),
        // CtxSplitLayout
        SmallVector<SecHdrTableEntry, 8>({{SecProfSummary, 0, 0, 0, 0},
                                          {SecNameTable, 0, 0, 0, 0},
                                          // profile with context
                                          // for next two sections
                                          {SecFuncOffsetTable, 0, 0, 0, 0},
                                          {SecLBRProfile, 0, 0, 0, 0},
                                          // profile without context
                                          // for next two sections
                                          {SecFuncOffsetTable, 0, 0, 0, 0},
                                          {SecLBRProfile, 0, 0, 0, 0},
                                          {SecProfileSymbolList, 0, 0, 0, 0},
                                          {SecFuncMetadata, 0, 0, 0, 0}}),
};

class SampleProfileWriterExtBinaryBase : public SampleProfileWriterBinary {
  using SampleProfileWriterBinary::SampleProfileWriterBinary;
public:
  std::error_code write(FunctionSamplesVector &ProfileMap) override;

  void setToCompressAllSections() override;
  void setToCompressSection(SecType Type);
  std::error_code writeSample(const FunctionSamples &S) override;

  // Set to use MD5 to represent string in NameTable.
  void setUseMD5() override {
    UseMD5 = true;
    addSectionFlag(SecNameTable, SecNameTableFlags::SecFlagMD5Name);
    // MD5 will be stored as plain uint64_t instead of variable-length
    // quantity format in NameTable section.
    addSectionFlag(SecNameTable, SecNameTableFlags::SecFlagFixedLengthMD5);
  }

  // Set the profile to be partial. It means the profile is for
  // common/shared code. The common profile is usually merged from
  // profiles collected from running other targets.
  void setPartialProfile() override {
    addSectionFlag(SecProfSummary, SecProfSummaryFlags::SecFlagPartial);
  }

  void setProfileSymbolList(ProfileSymbolList *PSL) override {
    ProfSymList = PSL;
  };

  void resetSecLayout(SectionLayout SL) override {
    verifySecLayout(SL);
#ifndef NDEBUG
    // Make sure resetSecLayout is called before any flag setting.
    for (auto &Entry : SectionHdrLayout) {
      assert(Entry.Flags == 0 &&
             "resetSecLayout has to be called before any flag setting");
    }
#endif
    SecLayout = SL;
    SectionHdrLayout = ExtBinaryHdrLayoutTable[SL];
  }

protected:
  uint64_t markSectionStart(SecType Type, uint32_t LayoutIdx);
  std::error_code addNewSection(SecType Sec, uint32_t LayoutIdx,
                                uint64_t SectionStart);
  template <class SecFlagType>
  void addSectionFlag(SecType Type, SecFlagType Flag) {
    for (auto &Entry : SectionHdrLayout) {
      if (Entry.Type == Type)
        addSecFlag(Entry, Flag);
    }
  }
  template <class SecFlagType>
  void addSectionFlag(uint32_t SectionIdx, SecFlagType Flag) {
    addSecFlag(SectionHdrLayout[SectionIdx], Flag);
  }

  void addContext(const SampleContext &Context) override;

  // placeholder for subclasses to dispatch their own section writers.
  virtual std::error_code writeCustomSection(SecType Type) = 0;
  // Verify the SecLayout is supported by the format.
  virtual void verifySecLayout(SectionLayout SL) = 0;

  // specify the order to write sections.
  virtual std::error_code writeSections(FunctionSamplesVector &ProfileMap) = 0;

  // Dispatch section writer for each section. \p LayoutIdx is the sequence
  // number indicating where the section is located in SectionHdrLayout.
  virtual std::error_code writeOneSection(SecType Type, uint32_t LayoutIdx,
                                          FunctionSamplesVector &ProfileMap);

  // Helper function to write name table.
  std::error_code writeNameTable() override;
  std::error_code writeContextIdx(const SampleContext &Context) override;
  std::error_code writeCSNameIdx(const SampleContext &Context);
  std::error_code writeCSNameTableSection();

  std::error_code writeFuncMetadata(FunctionSamplesVector &Profiles);
  std::error_code writeFuncMetadata(const FunctionSamples &Profile);

  // Functions to write various kinds of sections.
  std::error_code writeNameTableSection(FunctionSamplesVector &ProfileMap);
  std::error_code writeFuncOffsetTable();
  std::error_code writeProfileSymbolListSection();

  SectionLayout SecLayout = DefaultLayout;
  // Specifiy the order of sections in section header table. Note
  // the order of sections in SecHdrTable may be different that the
  // order in SectionHdrLayout. sample Reader will follow the order
  // in SectionHdrLayout to read each section.
  SmallVector<SecHdrTableEntry, 8> SectionHdrLayout =
      ExtBinaryHdrLayoutTable[DefaultLayout];

  // Save the start of SecLBRProfile so we can compute the offset to the
  // start of SecLBRProfile for each Function's Profile and will keep it
  // in FuncOffsetTable.
  uint64_t SecLBRProfileStart = 0;

private:
  void allocSecHdrTable();
  std::error_code writeSecHdrTable();
  std::error_code writeHeader(FunctionSamplesVector &ProfileMap) override;
  std::error_code compressAndOutput();

  // We will swap the raw_ostream held by LocalBufStream and that
  // held by OutputStream if we try to add a section which needs
  // compression. After the swap, all the data written to output
  // will be temporarily buffered into the underlying raw_string_ostream
  // originally held by LocalBufStream. After the data writing for the
  // section is completed, compress the data in the local buffer,
  // swap the raw_ostream back and write the compressed data to the
  // real output.
  std::unique_ptr<raw_ostream> LocalBufStream;
  // The location where the output stream starts.
  uint64_t FileStart;
  // The location in the output stream where the SecHdrTable should be
  // written to.
  uint64_t SecHdrTableOffset;
  // The table contains SecHdrTableEntry entries in order of how they are
  // populated in the writer. It may be different from the order in
  // SectionHdrLayout which specifies the sequence in which sections will
  // be read.
  std::vector<SecHdrTableEntry> SecHdrTable;

  // FuncOffsetTable maps function context to its profile offset in
  // SecLBRProfile section. It is used to load function profile on demand.
  MapVector<SampleContext, uint64_t> FuncOffsetTable;
  // Whether to use MD5 to represent string.
  bool UseMD5 = false;

  /// CSNameTable maps function context to its offset in SecCSNameTable section.
  /// The offset will be used everywhere where the context is referenced.
  MapVector<SampleContext, uint32_t> CSNameTable;

  ProfileSymbolList *ProfSymList = nullptr;
};

class SampleProfileWriterExtBinary : public SampleProfileWriterExtBinaryBase {
public:
  SampleProfileWriterExtBinary(std::unique_ptr<raw_ostream> &OS)
      : SampleProfileWriterExtBinaryBase(OS) {}

private:
  std::error_code writeDefaultLayout(FunctionSamplesVector &ProfileMap);
  std::error_code writeCtxSplitLayout(FunctionSamplesVector &ProfileMap);

  std::error_code writeSections(FunctionSamplesVector &ProfileMap) override;

  std::error_code writeCustomSection(SecType Type) override {
    return sampleprof_error::success;
  };

  void verifySecLayout(SectionLayout SL) override {
    assert((SL == DefaultLayout || SL == CtxSplitLayout) &&
           "Unsupported layout");
  }
};

} // end namespace sampleprof
} // end namespace llvm

#endif // LLVM_PROFILEDATA_SAMPLEPROFWRITER_H
