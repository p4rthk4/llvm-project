//===- LegalizeToF32.cpp - Legalize functions on small floats ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements legalizing math operations on small floating-point
// types through arith.extf and arith.truncf.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Math/Transforms/Passes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/STLExtras.h"

namespace mlir::math {
#define GEN_PASS_DEF_MATHLEGALIZETOF32
#include "mlir/Dialect/Math/Transforms/Passes.h.inc"
} // namespace mlir::math

using namespace mlir;
namespace {
struct LegalizeToF32RewritePattern final : ConversionPattern {
  LegalizeToF32RewritePattern(TypeConverter &converter, MLIRContext *context)
      : ConversionPattern(converter, MatchAnyOpTypeTag{}, 1, context) {}
  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

struct LegalizeToF32Pass final
    : mlir::math::impl::MathLegalizeToF32Base<LegalizeToF32Pass> {
  void runOnOperation() override;
};
} // namespace

void mlir::math::populateLegalizeToF32TypeConverter(
    TypeConverter &typeConverter) {
  typeConverter.addConversion(
      [](Type type) -> std::optional<Type> { return type; });
  typeConverter.addConversion([](FloatType type) -> std::optional<Type> {
    if (type.getWidth() < 32)
      return Float32Type::get(type.getContext());
    return std::nullopt;
  });
  typeConverter.addConversion([](ShapedType type) -> std::optional<Type> {
    if (auto elemTy = dyn_cast<FloatType>(type.getElementType()))
      return type.clone(Float32Type::get(type.getContext()));
    return std::nullopt;
  });
  typeConverter.addTargetMaterialization(
      [](OpBuilder &b, Type target, ValueRange input, Location loc) {
        auto extFOp = b.create<arith::ExtFOp>(loc, target, input);
        extFOp.setFastmath(arith::FastMathFlags::contract);
        return extFOp;
      });
}

void mlir::math::populateLegalizeToF32ConversionTarget(
    ConversionTarget &target, TypeConverter &typeConverter) {
  target.markUnknownOpDynamicallyLegal([&typeConverter](Operation *op) -> bool {
    if (isa<MathDialect>(op->getDialect()))
      return typeConverter.isLegal(op);
    return true;
  });
  target.addLegalOp<FmaOp>();
  target.addLegalOp<arith::ExtFOp, arith::TruncFOp>();
}

LogicalResult LegalizeToF32RewritePattern::matchAndRewrite(
    Operation *op, ArrayRef<Value> operands,
    ConversionPatternRewriter &rewriter) const {
  Location loc = op->getLoc();
  const TypeConverter *converter = getTypeConverter();
  FailureOr<Operation *> legalized =
      convertOpResultTypes(op, operands, *converter, rewriter);
  if (failed(legalized))
    return failure();

  SmallVector<Value> results = (*legalized)->getResults();
  for (auto [result, newType, origType] : llvm::zip_equal(
           results, (*legalized)->getResultTypes(), op->getResultTypes())) {
    if (newType != origType) {
      auto truncFOp = rewriter.create<arith::TruncFOp>(loc, origType, result);
      truncFOp.setFastmath(arith::FastMathFlags::contract);
      result = truncFOp.getResult();
    }
  }
  rewriter.replaceOp(op, results);
  return success();
}

void mlir::math::populateLegalizeToF32Patterns(RewritePatternSet &patterns,
                                               TypeConverter &typeConverter) {
  patterns.add<LegalizeToF32RewritePattern>(typeConverter,
                                            patterns.getContext());
}

void LegalizeToF32Pass::runOnOperation() {
  Operation *op = getOperation();
  MLIRContext &ctx = getContext();

  TypeConverter typeConverter;
  math::populateLegalizeToF32TypeConverter(typeConverter);
  ConversionTarget target(ctx);
  math::populateLegalizeToF32ConversionTarget(target, typeConverter);
  RewritePatternSet patterns(&ctx);
  math::populateLegalizeToF32Patterns(patterns, typeConverter);
  if (failed(applyPartialConversion(op, target, std::move(patterns))))
    return signalPassFailure();
}
