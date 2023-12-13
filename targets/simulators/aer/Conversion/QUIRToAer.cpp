//===- QUIRToAER.cpp - Convert QUIR to AER --------------*- C++ -*-===//
//
// (C) Copyright IBM 2023.
//
// This code is part of Qiskit.
//
// This code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions. You may obtain a copy of this license in the LICENSE.txt
// file in the root directory of this source tree.
//
// Any modifications or derivative works of this code must retain this
// copyright notice, and modified files need to carry a notice indicating
// that they have been altered from the originals.
//
//===----------------------------------------------------------------------===//
//
//  This file implements passes for converting QUIR to AER
//
//===----------------------------------------------------------------------===//
#include "Conversion/QUIRToAer.h"
#include "Conversion/OQ3ToStandard/OQ3ToStandard.h"
#include "Conversion/QUIRToStandard/VariablesToGlobalMemRefConversion.h"
#include "Conversion/TypeConversion.h"

#include "Dialect/OQ3/IR/OQ3Ops.h"
#include "Dialect/Pulse/IR/PulseDialect.h"
#include "Dialect/QCS/IR/QCSOps.h"
#include "Dialect/QUIR/IR/QUIRDialect.h"
#include "Dialect/QUIR/IR/QUIROps.h"
#include "Dialect/QUIR/Utils/Utils.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/StandardOps/Transforms/FuncConversions.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

using namespace mlir;

#include <exception>

namespace qssc::targets::simulators::aer::conversion {

namespace {

// The wrapper helps a converter access the global state by generating
// `AddressOp` and `LoadOp` automatically.
class AerStateWrapper {
public:
  AerStateWrapper() = default;
  AerStateWrapper(LLVM::GlobalOp mem) : mem(mem) {}

  Value access(OpBuilder &builder) const {
    auto addr = getAddr(builder);
    return builder.create<LLVM::LoadOp>(builder.getUnknownLoc(), addr,
                                        /*alignment=*/8);
  }

  Value getAddr(OpBuilder &builder) const {
    return builder.create<LLVM::AddressOfOp>(builder.getUnknownLoc(), mem);
  }

  LLVM::GlobalOp raw() const { return mem; }

private:
  LLVM::GlobalOp mem;
};

using AerFunctionTable = std::map<std::string, LLVM::LLVMFuncOp>;
// Declare Aer runtime API functions globally.
// The definitions of those functions are given externally by a linker.
AerFunctionTable declareAerFunctions(ModuleOp moduleOp) {
  using LLVM::LLVMFunctionType;

  AerFunctionTable aerFuncTable;
  OpBuilder builder(moduleOp);

  auto registerFunc = [&](const char *name, LLVMFunctionType ty) {
    const auto loc = builder.getUnknownLoc();
    const auto f = builder.create<LLVM::LLVMFuncOp>(loc, name, ty);
    aerFuncTable.insert({name, f});
  };

  auto *context = moduleOp->getContext();
  builder.setInsertionPointToStart(moduleOp.getBody());
  // common types
  const auto voidType = LLVM::LLVMVoidType::get(context);
  const auto i8Type = builder.getI8Type();
  const auto i64Type = builder.getI64Type();
  const auto f64Type = builder.getF64Type();
  const auto aerStateType = LLVM::LLVMPointerType::get(i8Type);
  const auto strType = LLVM::LLVMPointerType::get(i8Type);
  // @aer_state(...) -> i8*
  const auto aerStateFunType = LLVMFunctionType::get(aerStateType, {}, true);
  registerFunc("aer_state", aerStateFunType);
  // @aer_state_configure(i8* noundef, i8* noundef, i8* noundef) -> void
  const auto aerStateConfigureType =
      LLVMFunctionType::get(voidType, {strType, strType, strType});
  registerFunc("aer_state_configure", aerStateConfigureType);
  // @aer_allocate_qubits(i8* noundef, i64 noundef) -> i64
  const auto aerAllocQubitsType =
      LLVMFunctionType::get(i64Type, {aerStateType, i64Type});
  registerFunc("aer_allocate_qubits", aerAllocQubitsType);
  // @aer_state_initialize(i8*) -> i8*
  const auto aerStateInitType =
      LLVMFunctionType::get(aerStateType, {aerStateType});
  registerFunc("aer_state_initialize", aerStateInitType);
  // @aer_apply_u3(i8* noundef, i64 noundef, i64 noundef, i64 noundef) -> void
  const auto aerApplyU3Type = LLVMFunctionType::get(
      voidType, {aerStateType, i64Type, f64Type, f64Type, f64Type});
  registerFunc("aer_apply_u3", aerApplyU3Type);
  // @aer_apply_cx(i8* noundef, i64 noundef, i64 noundef) -> void
  const auto aerApplyCXType =
      LLVMFunctionType::get(voidType, {aerStateType, i64Type, i64Type});
  registerFunc("aer_apply_cx", aerApplyCXType);
  // @aer_apply_measure(i8* noundef, i64* noundef, i64 noundef) -> i64
  const auto aerMeasType = LLVMFunctionType::get(
      i64Type, {aerStateType, LLVM::LLVMPointerType::get(i64Type), i64Type});
  registerFunc("aer_apply_measure", aerMeasType);
  // @aer_state_finalize(i8* noundef) -> void
  const auto aerStateFinalizeType =
      LLVMFunctionType::get(voidType, aerStateType);
  registerFunc("aer_state_finalize", aerStateFinalizeType);

  return aerFuncTable;
}

// Create an Aer state globally and then wrap the state value.
AerStateWrapper createAerState(MLIRContext *ctx, ModuleOp moduleOp,
                               AerFunctionTable aerFuncTable) {
  const auto i8Type = IntegerType::get(ctx, 8);

  OpBuilder builder(moduleOp);
  builder.setInsertionPointToStart(moduleOp.getBody());
  auto aerStateTy = LLVM::LLVMPointerType::get(i8Type);
  auto globalState = builder.create<LLVM::GlobalOp>(
      moduleOp->getLoc(), aerStateTy, /*isConstant=*/false, LLVM::Linkage::Weak,
      "aer_state_handler", Attribute{},
      /*alignment=*/8);
  auto aerState = AerStateWrapper(globalState);

  auto *mainFunc = mlir::quir::getMainFunction(moduleOp);
  auto *mainBody = &mainFunc->getRegion(0).getBlocks().front();
  builder.setInsertionPointToStart(mainBody);
  auto addr = aerState.getAddr(builder);
  auto call =
      builder
          .create<LLVM::CallOp>(builder.getUnknownLoc(),
                                aerFuncTable.at("aer_state"), ValueRange{})
          .getResult(0);
  builder.create<LLVM::StoreOp>(builder.getUnknownLoc(), call, addr);

  return aerState;
}

void insertAerStateInitialize(ModuleOp moduleOp, AerStateWrapper aerState,
                              AerFunctionTable aerFuncTable) {
  OpBuilder builder(moduleOp);

  // Insert Aer runtime initialization after qubit declarations.
  // Assume that the following conditions hold:
  //   1. Each qubit declaration has a unique id (e.g., {id = 0 : i32}).
  //   2. The last qubit declaration has the biggest id.
  std::optional<quir::DeclareQubitOp> lastQubitDeclOp;
  moduleOp.walk([&](quir::DeclareQubitOp declOp) {
    if (!lastQubitDeclOp ||
        lastQubitDeclOp->id().getValue() < declOp.id().getValue())
      lastQubitDeclOp = declOp;
  });
  assert(lastQubitDeclOp && "At least one qubit must be declared.");
  builder.setInsertionPointAfter(*lastQubitDeclOp);
  auto state = aerState.access(builder);
  builder.create<LLVM::CallOp>(lastQubitDeclOp->getLoc(),
                               aerFuncTable.at("aer_state_initialize"), state);
}

using ArrayForMeas = mlir::Value;
// Allocate an array for measurements globally.
// @note Aer C API requires an array of measured qubits. This provides a common
// array for the measurements that can avoid a stack allocation for each
// function call of the Aer measurement.
// Note that the size of this array must be large enough to perform all
// the measurements appeared in a given program.
ArrayForMeas prepareArrayForMeas(ModuleOp moduleOp) {
  OpBuilder builder(moduleOp);

  const auto i64Type = builder.getI64Type();
  auto mainFunc = mlir::quir::getMainFunction(moduleOp);
  builder.setInsertionPointToStart(&mainFunc->getRegion(0).getBlocks().front());
  const int arraySize = 1; // TODO: Support multi-body measurement in future
  auto arrSizeOp = builder.create<arith::ConstantOp>(
      builder.getUnknownLoc(), i64Type,
      builder.getIntegerAttr(i64Type, arraySize));
  return builder
      .create<LLVM::AllocaOp>(builder.getUnknownLoc(),
                              LLVM::LLVMPointerType::get(i64Type), arrSizeOp,
                              /*alignment=*/8)
      .getResult();
}

} // namespace

// Assume qcs.init is called before all quir.declare_qubit operations
struct QCSInitConversionPat : public OpConversionPattern<qcs::SystemInitOp> {
  explicit QCSInitConversionPat(MLIRContext *ctx, TypeConverter &typeConverter,
                                AerSimulatorConfig config,
                                AerStateWrapper aerState,
                                AerFunctionTable aerFuncTable)
      : OpConversionPattern(typeConverter, ctx, /* benefit= */ 1),
        config(std::move(config)), aerFuncTable(std::move(aerFuncTable)),
        aerState(aerState) {}

  LogicalResult
  matchAndRewrite(qcs::SystemInitOp initOp, qcs::SystemInitOp::Adaptor adapter,
                  ConversionPatternRewriter &rewriter) const override {

    // global string values for aer configuration
    std::map<std::string, mlir::Value> globals;
    const auto method = config.getMethod();
    const auto device = config.getDevice();
    const auto precision = config.getPrecision();
    const auto *const methodStr = toStringInAer(method);
    const auto *const deviceStr = toStringInAer(device);
    const auto *const precisionStr = toStringInAer(precision);
    const auto config_strs = {"method",  methodStr,   "device",
                              deviceStr, "precision", precisionStr};
    for (const auto *config_str : config_strs) {
      const auto var_name = std::string("aer_conf_") + config_str;
      const auto with_null = config_str + std::string("\0", 1);
      globals[config_str] =
          LLVM::createGlobalString(initOp->getLoc(), rewriter, var_name,
                                   with_null, LLVM::Linkage::Private);
    }
    // configure
    // aer_state_configure(state, "method", <given method in .cfg>)
    // aer_state_configure(state, "device", <given device in .cfg>)
    // aer_state_configure(state, "precision", <given precision in .cfg>)
    auto state = aerState.access(rewriter);
    rewriter.create<LLVM::CallOp>(
        initOp->getLoc(), aerFuncTable.at("aer_state_configure"),
        ValueRange{state, globals["method"], globals[methodStr]});
    rewriter.create<LLVM::CallOp>(
        initOp->getLoc(), aerFuncTable.at("aer_state_configure"),
        ValueRange{state, globals["device"], globals[deviceStr]});
    rewriter.create<LLVM::CallOp>(
        initOp->getLoc(), aerFuncTable.at("aer_state_configure"),
        ValueRange{state, globals["precision"], globals[precisionStr]});
    rewriter.eraseOp(initOp);
    return success();
  }

private:
  const AerSimulatorConfig config;
  const AerFunctionTable aerFuncTable;
  AerStateWrapper aerState;
};

// Currently the simulator target does not support shot iterations.
struct RemoveQCSShotInitConversionPat
    : public OpConversionPattern<qcs::ShotInitOp> {
  explicit RemoveQCSShotInitConversionPat(MLIRContext *ctx,
                                          TypeConverter &typeConverter)
      : OpConversionPattern(typeConverter, ctx, /* benefit= */ 1) {}

  LogicalResult
  matchAndRewrite(qcs::ShotInitOp initOp, qcs::ShotInitOp::Adaptor adapter,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.eraseOp(initOp);
    return success();
  }
};

struct FinalizeConversionPat
    : public OpConversionPattern<qcs::SystemFinalizeOp> {
  explicit FinalizeConversionPat(MLIRContext *ctx, TypeConverter &typeConverter,
                                 AerStateWrapper aerState,
                                 AerFunctionTable aerFuncTable)
      : OpConversionPattern(typeConverter, ctx, /* benefit= */ 1),
        aerState(aerState), aerFuncTable(std::move(aerFuncTable)) {}

  LogicalResult
  matchAndRewrite(qcs::SystemFinalizeOp finOp,
                  qcs::SystemFinalizeOp::Adaptor adapter,
                  ConversionPatternRewriter &rewriter) const override {
    PatternRewriter::InsertionGuard insertGuard(rewriter);
    rewriter.setInsertionPointAfter(finOp);
    rewriter.create<LLVM::CallOp>(rewriter.getUnknownLoc(),
                                  aerFuncTable.at("aer_state_finalize"),
                                  aerState.access(rewriter));
    rewriter.eraseOp(finOp);
    return success();
  }

private:
  AerStateWrapper aerState;
  const AerFunctionTable aerFuncTable;
};

struct DeclareQubitConversionPat
    : public OpConversionPattern<quir::DeclareQubitOp> {
  using Adaptor = quir::DeclareQubitOp::Adaptor;

  explicit DeclareQubitConversionPat(MLIRContext *ctx,
                                     TypeConverter &typeConverter,
                                     AerStateWrapper aerState,
                                     AerFunctionTable aerFuncTable)
      : OpConversionPattern(typeConverter, ctx, /*benefit=*/1),
        aerState(aerState), aerFuncTable(std::move(aerFuncTable)) {}

  LogicalResult
  matchAndRewrite(quir::DeclareQubitOp op, Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    const int width = op.getType().dyn_cast<quir::QubitType>().getWidth();
    assert(width == 1 && "Multi qubit declaration has not supported yet.");

    const auto sizeAttr = rewriter.getIntegerAttr(rewriter.getI64Type(), width);
    auto constOp = rewriter.create<arith::ConstantOp>(
        op->getLoc(), rewriter.getI64Type(), sizeAttr);
    auto state = aerState.access(rewriter);
    auto alloc = rewriter.create<LLVM::CallOp>(
        op->getLoc(), aerFuncTable.at("aer_allocate_qubits"),
        ValueRange{state, constOp});
    rewriter.replaceOp(op, alloc.getResults());
    return success();
  }

private:
  AerStateWrapper aerState;
  const AerFunctionTable aerFuncTable;
};

struct BuiltinUopConversionPat : public OpConversionPattern<quir::Builtin_UOp> {
  explicit BuiltinUopConversionPat(MLIRContext *ctx,
                                   TypeConverter &typeConverter,
                                   AerStateWrapper aerState,
                                   AerFunctionTable aerFuncTable)
      : OpConversionPattern(typeConverter, ctx, /*benefit=*/1),
        aerState(aerState), aerFuncTable(std::move(aerFuncTable)) {}

  LogicalResult
  matchAndRewrite(quir::Builtin_UOp op, quir::Builtin_UOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto state = aerState.access(rewriter);
    std::vector<Value> args = {state};
    args.insert(args.end(), adaptor.getOperands().begin(),
                adaptor.getOperands().end());
    rewriter.create<LLVM::CallOp>(op.getLoc(), aerFuncTable.at("aer_apply_u3"),
                                  args);
    rewriter.eraseOp(op);
    return success();
  }

private:
  AerStateWrapper aerState;
  const AerFunctionTable aerFuncTable;
};

struct BuiltinCXConversionPat : public OpConversionPattern<quir::BuiltinCXOp> {
  explicit BuiltinCXConversionPat(MLIRContext *ctx,
                                  TypeConverter &typeConverter,
                                  AerStateWrapper aerState,
                                  AerFunctionTable aerFuncTable)
      : OpConversionPattern(typeConverter, ctx, /*benefit=*/1),
        aerState(aerState), aerFuncTable(std::move(aerFuncTable)) {}

  LogicalResult
  matchAndRewrite(quir::BuiltinCXOp op, quir::BuiltinCXOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto state = aerState.access(rewriter);
    std::vector<Value> args = {state};
    args.insert(args.end(), adaptor.getOperands().begin(),
                adaptor.getOperands().end());
    rewriter.create<LLVM::CallOp>(op->getLoc(), aerFuncTable.at("aer_apply_cx"),
                                  args);
    rewriter.eraseOp(op);
    return success();
  }

private:
  AerStateWrapper aerState;
  const AerFunctionTable aerFuncTable;
};

struct MeasureOpConversionPat : public OpConversionPattern<quir::MeasureOp> {
  explicit MeasureOpConversionPat(MLIRContext *ctx,
                                  TypeConverter &typeConverter,
                                  AerStateWrapper aerState,
                                  AerFunctionTable aerFuncTable,
                                  ArrayForMeas arrayForMeas)
      : OpConversionPattern(typeConverter, ctx, /*benefit=*/1),
        aerState(aerState), aerFuncTable(std::move(aerFuncTable)),
        arrayForMeas(arrayForMeas) {}

  LogicalResult
  matchAndRewrite(quir::MeasureOp op, quir::MeasureOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    assert(op->getNumOperands() == 1 &&
           "Multi-body measurement have not been supported yet.");
    // How to cast?
    auto allocaArrForMeas =
        llvm::dyn_cast<LLVM::AllocaOp>(arrayForMeas.getDefiningOp());
    const auto i64Type = rewriter.getI64Type();
    const unsigned arrSize = 1; // TODO
    const IntegerAttr arraySizeAttr = rewriter.getIntegerAttr(i64Type, arrSize);
    const auto qubit = *adaptor.getOperands().begin();
    auto arrSizeOp = rewriter.create<arith::ConstantOp>(op->getLoc(), i64Type,
                                                        arraySizeAttr);
    rewriter.create<LLVM::StoreOp>(op->getLoc(), qubit, allocaArrForMeas);
    auto state = aerState.access(rewriter);
    auto meas = rewriter.create<LLVM::CallOp>(
        op->getLoc(), aerFuncTable.at("aer_apply_measure"),
        ValueRange{state, arrayForMeas, arrSizeOp});
    auto casted = rewriter.create<arith::TruncIOp>(
        op->getLoc(), meas.getResult(0), rewriter.getI1Type());
    rewriter.replaceOp(op, casted.getResult());

    return success();
  }

private:
  AerStateWrapper aerState;
  const AerFunctionTable aerFuncTable;
  ArrayForMeas arrayForMeas;
};

struct ConstConversionPat : public OpConversionPattern<quir::ConstantOp> {
  explicit ConstConversionPat(MLIRContext *ctx, TypeConverter &typeConverter)
      : OpConversionPattern(typeConverter, ctx, 1) {}

  LogicalResult
  matchAndRewrite(quir::ConstantOp op, quir::ConstantOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (auto angleAttr = op.value().dyn_cast<quir::AngleAttr>()) {
      rewriter.setInsertionPointAfter(op);
      const auto angle = angleAttr.getValue().convertToDouble();
      const auto fType = rewriter.getF64Type();
      FloatAttr fAttr = rewriter.getFloatAttr(fType, angle);
      auto constOp =
          rewriter.create<arith::ConstantOp>(op->getLoc(), fType, fAttr);
      rewriter.replaceOp(op, {constOp});
    } else if (op.value().isa<quir::DurationAttr>()) {
      rewriter.eraseOp(op);
    }
    return success();
  }
};

template <typename Op>
struct RemoveConversionPat : public OpConversionPattern<Op> {
  using Adaptor = typename Op::Adaptor;

  explicit RemoveConversionPat(MLIRContext *ctx, TypeConverter &typeConverter)
      : OpConversionPattern<Op>(typeConverter, ctx, 1) {}

  LogicalResult
  matchAndRewrite(Op op, Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};

// TDOO: Supporting custom gates is future work.
struct FunctionConversionPat : public OpConversionPattern<mlir::FuncOp> {
  explicit FunctionConversionPat(MLIRContext *ctx, TypeConverter &typeConverter)
      : OpConversionPattern(typeConverter, ctx, 1) {}

  LogicalResult
  matchAndRewrite(mlir::FuncOp funcOp, mlir::FuncOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Assume: funcOp != mainFunc
    if (funcOp.getName() == "main")
      return success();

    rewriter.eraseOp(funcOp);
    return success();
  }
};

void conversion::QUIRToAERPass::getDependentDialects(
    DialectRegistry &registry) const {
  registry.insert<LLVM::LLVMDialect, mlir::memref::MemRefDialect,
                  mlir::AffineDialect, arith::ArithmeticDialect>();
}

void QUIRToAERPass::runOnOperation(AerSimulator &system) {
  ModuleOp moduleOp = getOperation();
  const auto simulatorConfig = system.getConfig();

  // First remove all arguments from synchronization ops
  moduleOp->walk([](qcs::SynchronizeOp synchOp) {
    synchOp.qubitsMutable().assign(ValueRange({}));
  });

  AerTypeConverter typeConverter;
  auto *context = &getContext();
  ConversionTarget target(*context);

  target.addLegalDialect<arith::ArithmeticDialect, LLVM::LLVMDialect,
                         mlir::AffineDialect, memref::MemRefDialect,
                         scf::SCFDialect, StandardOpsDialect,
                         mlir::pulse::PulseDialect>();
  target
      .addIllegalDialect<qcs::QCSDialect, oq3::OQ3Dialect, quir::QUIRDialect>();
  target.addDynamicallyLegalOp<FuncOp>(
      [&](FuncOp op) { return typeConverter.isSignatureLegal(op.getType()); });

  // Aer initialization
  auto aerFuncTable = declareAerFunctions(moduleOp);
  auto aerState = createAerState(context, moduleOp, aerFuncTable);
  insertAerStateInitialize(moduleOp, aerState, aerFuncTable);
  auto arrayForMeas = prepareArrayForMeas(moduleOp);

  RewritePatternSet patterns(context);
  populateFunctionOpInterfaceTypeConversionPattern<FuncOp>(patterns,
                                                           typeConverter);
  populateCallOpTypeConversionPattern(patterns, typeConverter);
  oq3::populateOQ3ToStandardConversionPatterns(typeConverter, patterns);
  patterns.add<QCSInitConversionPat>(context, typeConverter, simulatorConfig,
                                     aerState, aerFuncTable);
  patterns.add<DeclareQubitConversionPat, FinalizeConversionPat,
               BuiltinUopConversionPat, BuiltinCXConversionPat>(
      context, typeConverter, aerState, aerFuncTable);
  patterns.add<MeasureOpConversionPat>(context, typeConverter, aerState,
                                       aerFuncTable, arrayForMeas);
  patterns.add<
      RemoveQCSShotInitConversionPat, ConstConversionPat, FunctionConversionPat,
      RemoveConversionPat<quir::DelayOp>,     // TODO: Support noise models
      RemoveConversionPat<quir::BarrierOp>,   // TODO: Support noise models
      RemoveConversionPat<quir::CallGateOp>>( // TODO: Support custom gates
      context, typeConverter);

  // With the target and rewrite patterns defined, we can now attempt the
  // conversion. The conversion will signal failure if any of our `illegal`
  // operations were not converted successfully.
  if (failed(applyPartialConversion(moduleOp, target, std::move(patterns))))
    llvm::outs() << "Failed applyPartialConversion\n";
} // QUIRToStdPass::runOnOperation()

llvm::StringRef QUIRToAERPass::getArgument() const {
  return "simulator-quir-to-aer";
}

llvm::StringRef QUIRToAERPass::getDescription() const {
  return "Convert QUIR ops to aer";
}

} // namespace qssc::targets::simulators::aer::conversion