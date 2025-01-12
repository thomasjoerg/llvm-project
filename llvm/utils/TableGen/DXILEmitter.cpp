//===- DXILEmitter.cpp - DXIL operation Emitter ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// DXILEmitter uses the descriptions of DXIL operation to construct enum and
// helper functions for DXIL operation.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"

using namespace llvm;

namespace {

struct DXILShaderModel {
  int Major;
  int Minor;
};
struct DXILParam {
  int Pos;        // position in parameter list
  StringRef Type; // llvm type name, $o for overload, $r for resource
                  // type, $cb for legacy cbuffer, $u4 for u4 struct
  StringRef Name; // short, unique name
  StringRef Doc;  // the documentation description of this parameter
  bool IsConst;   // whether this argument requires a constant value in the IR
  StringRef EnumName; // the name of the enum type if applicable
  int MaxValue;       // the maximum value for this parameter if applicable
  DXILParam(const Record *R) {
    Name = R->getValueAsString("name");
    Pos = R->getValueAsInt("pos");
    Type = R->getValueAsString("llvm_type");
    if (R->getValue("doc"))
      Doc = R->getValueAsString("doc");
    IsConst = R->getValueAsBit("is_const");
    EnumName = R->getValueAsString("enum_name");
    MaxValue = R->getValueAsInt("max_value");
  }
};

struct DXILOperationData {
  StringRef Name; // short, unique name

  StringRef DXILOp;    // name of DXIL operation
  int DXILOpID;        // ID of DXIL operation
  StringRef DXILClass; // name of the opcode class
  StringRef Category;  // classification for this instruction
  StringRef Doc;       // the documentation description of this instruction

  SmallVector<DXILParam> Params; // the operands that this instruction takes
  StringRef OverloadTypes;       // overload types if applicable
  StringRef FnAttr;              // attribute shorthands: rn=does not access
                                 // memory,ro=only reads from memory,
  bool IsDeriv;                  // whether this is some kind of derivative
  bool IsGradient;               // whether this requires a gradient calculation
  bool IsFeedback;               // whether this is a sampler feedback op
  bool IsWave; // whether this requires in-wave, cross-lane functionality
  bool RequiresUniformInputs; // whether this operation requires that all
                              // of its inputs are uniform across the wave
  SmallVector<StringRef, 4>
      ShaderStages; // shader stages to which this applies, empty for all.
  DXILShaderModel ShaderModel;           // minimum shader model required
  DXILShaderModel ShaderModelTranslated; // minimum shader model required with
                                         // translation by linker
  SmallVector<StringRef, 4> counters;    // counters for this inst.
  DXILOperationData(const Record *R) {
    Name = R->getValueAsString("name");
    DXILOp = R->getValueAsString("dxil_op");
    DXILOpID = R->getValueAsInt("dxil_opid");
    DXILClass = R->getValueAsDef("op_class")->getValueAsString("name");
    Category = R->getValueAsDef("category")->getValueAsString("name");

    Doc = R->getValueAsString("doc");
    ListInit *ParamList = R->getValueAsListInit("ops");
    for (unsigned i = 0; i < ParamList->size(); ++i) {
      Record *Param = ParamList->getElementAsRecord(i);
      Params.emplace_back(DXILParam(Param));
    }
    OverloadTypes = R->getValueAsString("oload_types");
    FnAttr = R->getValueAsString("fn_attr");
  }
};
} // end anonymous namespace

static void emitDXILOpEnum(DXILOperationData &DXILOp, raw_ostream &OS) {
  // Name = ID, // Doc
  OS << DXILOp.Name << " = " << DXILOp.DXILOpID << ", // " << DXILOp.Doc
     << "\n";
}

static std::string buildCategoryStr(StringSet<> &Cetegorys) {
  std::string Str;
  raw_string_ostream OS(Str);
  for (auto &It : Cetegorys) {
    OS << " " << It.getKey();
  }
  return OS.str();
}

// Emit enum declaration for DXIL.
static void emitDXILEnums(std::vector<DXILOperationData> &DXILOps,
                          raw_ostream &OS) {
  // Sort by Category + OpName.
  std::sort(DXILOps.begin(), DXILOps.end(),
            [](DXILOperationData &A, DXILOperationData &B) {
              // Group by Category first.
              if (A.Category == B.Category)
                // Inside same Category, order by OpName.
                return A.DXILOp < B.DXILOp;
              else
                return A.Category < B.Category;
            });

  OS << "// Enumeration for operations specified by DXIL\n";
  OS << "enum class OpCode : unsigned {\n";

  StringMap<StringSet<>> ClassMap;
  StringRef PrevCategory = "";
  for (auto &DXILOp : DXILOps) {
    StringRef Category = DXILOp.Category;
    if (Category != PrevCategory) {
      OS << "\n// " << Category << "\n";
      PrevCategory = Category;
    }
    emitDXILOpEnum(DXILOp, OS);
    auto It = ClassMap.find(DXILOp.DXILClass);
    if (It != ClassMap.end()) {
      It->second.insert(DXILOp.Category);
    } else {
      ClassMap[DXILOp.DXILClass].insert(DXILOp.Category);
    }
  }

  OS << "\n};\n\n";

  std::vector<std::pair<std::string, std::string>> ClassVec;
  for (auto &It : ClassMap) {
    ClassVec.emplace_back(
        std::make_pair(It.getKey().str(), buildCategoryStr(It.second)));
  }
  // Sort by Category + ClassName.
  std::sort(ClassVec.begin(), ClassVec.end(),
            [](std::pair<std::string, std::string> &A,
               std::pair<std::string, std::string> &B) {
              StringRef ClassA = A.first;
              StringRef CategoryA = A.second;
              StringRef ClassB = B.first;
              StringRef CategoryB = B.second;
              // Group by Category first.
              if (CategoryA == CategoryB)
                // Inside same Category, order by ClassName.
                return ClassA < ClassB;
              else
                return CategoryA < CategoryB;
            });

  OS << "// Groups for DXIL operations with equivalent function templates\n";
  OS << "enum class OpCodeClass : unsigned {\n";
  PrevCategory = "";
  for (auto &It : ClassVec) {

    StringRef Category = It.second;
    if (Category != PrevCategory) {
      OS << "\n// " << Category << "\n";
      PrevCategory = Category;
    }
    StringRef Name = It.first;
    OS << Name << ",\n";
  }
  OS << "\n};\n\n";
}

namespace llvm {

void EmitDXILOperation(RecordKeeper &Records, raw_ostream &OS) {
  std::vector<Record *> Ops = Records.getAllDerivedDefinitions("dxil_op");
  OS << "// Generated code, do not edit.\n";
  OS << "\n";

  std::vector<DXILOperationData> DXILOps;
  DXILOps.reserve(Ops.size());
  for (auto *Record : Ops) {
    DXILOps.emplace_back(DXILOperationData(Record));
  }

  OS << "#ifdef DXIL_OP_ENUM\n";
  emitDXILEnums(DXILOps, OS);
  OS << "#endif\n\n";

  OS << "\n";
}

} // namespace llvm
