//===--- SemaOpenACC.cpp - Semantic Analysis for OpenACC constructs -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements semantic analysis for OpenACC constructs and
/// clauses.
///
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaOpenACC.h"
#include "clang/AST/StmtOpenACC.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/OpenACCKinds.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Casting.h"

using namespace clang;

namespace {
bool diagnoseConstructAppertainment(SemaOpenACC &S, OpenACCDirectiveKind K,
                                    SourceLocation StartLoc, bool IsStmt) {
  switch (K) {
  default:
  case OpenACCDirectiveKind::Invalid:
    // Nothing to do here, both invalid and unimplemented don't really need to
    // do anything.
    break;
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::Kernels:
    if (!IsStmt)
      return S.Diag(StartLoc, diag::err_acc_construct_appertainment) << K;
    break;
  }
  return false;
}

bool doesClauseApplyToDirective(OpenACCDirectiveKind DirectiveKind,
                                OpenACCClauseKind ClauseKind) {
  switch (ClauseKind) {
    // FIXME: For each clause as we implement them, we can add the
    // 'legalization' list here.
  case OpenACCClauseKind::Default:
    switch (DirectiveKind) {
    case OpenACCDirectiveKind::Parallel:
    case OpenACCDirectiveKind::Serial:
    case OpenACCDirectiveKind::Kernels:
    case OpenACCDirectiveKind::ParallelLoop:
    case OpenACCDirectiveKind::SerialLoop:
    case OpenACCDirectiveKind::KernelsLoop:
    case OpenACCDirectiveKind::Data:
      return true;
    default:
      return false;
    }
  case OpenACCClauseKind::If:
    switch (DirectiveKind) {
    case OpenACCDirectiveKind::Parallel:
    case OpenACCDirectiveKind::Serial:
    case OpenACCDirectiveKind::Kernels:
    case OpenACCDirectiveKind::Data:
    case OpenACCDirectiveKind::EnterData:
    case OpenACCDirectiveKind::ExitData:
    case OpenACCDirectiveKind::HostData:
    case OpenACCDirectiveKind::Init:
    case OpenACCDirectiveKind::Shutdown:
    case OpenACCDirectiveKind::Set:
    case OpenACCDirectiveKind::Update:
    case OpenACCDirectiveKind::Wait:
    case OpenACCDirectiveKind::ParallelLoop:
    case OpenACCDirectiveKind::SerialLoop:
    case OpenACCDirectiveKind::KernelsLoop:
      return true;
    default:
      return false;
    }
  case OpenACCClauseKind::Self:
    switch (DirectiveKind) {
    case OpenACCDirectiveKind::Parallel:
    case OpenACCDirectiveKind::Serial:
    case OpenACCDirectiveKind::Kernels:
    case OpenACCDirectiveKind::Update:
    case OpenACCDirectiveKind::ParallelLoop:
    case OpenACCDirectiveKind::SerialLoop:
    case OpenACCDirectiveKind::KernelsLoop:
      return true;
    default:
      return false;
    }
  case OpenACCClauseKind::NumGangs:
  case OpenACCClauseKind::NumWorkers:
  case OpenACCClauseKind::VectorLength:
    switch (DirectiveKind) {
    case OpenACCDirectiveKind::Parallel:
    case OpenACCDirectiveKind::Kernels:
    case OpenACCDirectiveKind::ParallelLoop:
    case OpenACCDirectiveKind::KernelsLoop:
      return true;
    default:
      return false;
    }
  case OpenACCClauseKind::Private:
    switch (DirectiveKind) {
    case OpenACCDirectiveKind::Parallel:
    case OpenACCDirectiveKind::Serial:
    case OpenACCDirectiveKind::Loop:
    case OpenACCDirectiveKind::ParallelLoop:
    case OpenACCDirectiveKind::SerialLoop:
    case OpenACCDirectiveKind::KernelsLoop:
      return true;
    default:
      return false;
    }
  default:
    // Do nothing so we can go to the 'unimplemented' diagnostic instead.
    return true;
  }
  llvm_unreachable("Invalid clause kind");
}

bool checkAlreadyHasClauseOfKind(
    SemaOpenACC &S, ArrayRef<const OpenACCClause *> ExistingClauses,
    SemaOpenACC::OpenACCParsedClause &Clause) {
  const auto *Itr = llvm::find_if(ExistingClauses, [&](const OpenACCClause *C) {
    return C->getClauseKind() == Clause.getClauseKind();
  });
  if (Itr != ExistingClauses.end()) {
    S.Diag(Clause.getBeginLoc(), diag::err_acc_duplicate_clause_disallowed)
        << Clause.getDirectiveKind() << Clause.getClauseKind();
    S.Diag((*Itr)->getBeginLoc(), diag::note_acc_previous_clause_here);
    return true;
  }
  return false;
}

} // namespace

SemaOpenACC::SemaOpenACC(Sema &S) : SemaBase(S) {}

OpenACCClause *
SemaOpenACC::ActOnClause(ArrayRef<const OpenACCClause *> ExistingClauses,
                         OpenACCParsedClause &Clause) {
  if (Clause.getClauseKind() == OpenACCClauseKind::Invalid)
    return nullptr;

  // Diagnose that we don't support this clause on this directive.
  if (!doesClauseApplyToDirective(Clause.getDirectiveKind(),
                                  Clause.getClauseKind())) {
    Diag(Clause.getBeginLoc(), diag::err_acc_clause_appertainment)
        << Clause.getDirectiveKind() << Clause.getClauseKind();
    return nullptr;
  }

  switch (Clause.getClauseKind()) {
  case OpenACCClauseKind::Default: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // Don't add an invalid clause to the AST.
    if (Clause.getDefaultClauseKind() == OpenACCDefaultClauseKind::Invalid)
      return nullptr;

    // OpenACC 3.3, Section 2.5.4:
    // At most one 'default' clause may appear, and it must have a value of
    // either 'none' or 'present'.
    // Second half of the sentence is diagnosed during parsing.
    if (checkAlreadyHasClauseOfKind(*this, ExistingClauses, Clause))
      return nullptr;

    return OpenACCDefaultClause::Create(
        getASTContext(), Clause.getDefaultClauseKind(), Clause.getBeginLoc(),
        Clause.getLParenLoc(), Clause.getEndLoc());
  }

  case OpenACCClauseKind::If: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // There is no prose in the standard that says duplicates aren't allowed,
    // but this diagnostic is present in other compilers, as well as makes
    // sense.
    if (checkAlreadyHasClauseOfKind(*this, ExistingClauses, Clause))
      return nullptr;

    // The parser has ensured that we have a proper condition expr, so there
    // isn't really much to do here.

    // If the 'if' clause is true, it makes the 'self' clause have no effect,
    // diagnose that here.
    // TODO OpenACC: When we add these two to other constructs, we might not
    // want to warn on this (for example, 'update').
    const auto *Itr =
        llvm::find_if(ExistingClauses, llvm::IsaPred<OpenACCSelfClause>);
    if (Itr != ExistingClauses.end()) {
      Diag(Clause.getBeginLoc(), diag::warn_acc_if_self_conflict);
      Diag((*Itr)->getBeginLoc(), diag::note_acc_previous_clause_here);
    }

    return OpenACCIfClause::Create(
        getASTContext(), Clause.getBeginLoc(), Clause.getLParenLoc(),
        Clause.getConditionExpr(), Clause.getEndLoc());
  }

  case OpenACCClauseKind::Self: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // TODO OpenACC: When we implement this for 'update', this takes a
    // 'var-list' instead of a condition expression, so semantics/handling has
    // to happen differently here.

    // There is no prose in the standard that says duplicates aren't allowed,
    // but this diagnostic is present in other compilers, as well as makes
    // sense.
    if (checkAlreadyHasClauseOfKind(*this, ExistingClauses, Clause))
      return nullptr;

    // If the 'if' clause is true, it makes the 'self' clause have no effect,
    // diagnose that here.
    // TODO OpenACC: When we add these two to other constructs, we might not
    // want to warn on this (for example, 'update').
    const auto *Itr =
        llvm::find_if(ExistingClauses, llvm::IsaPred<OpenACCIfClause>);
    if (Itr != ExistingClauses.end()) {
      Diag(Clause.getBeginLoc(), diag::warn_acc_if_self_conflict);
      Diag((*Itr)->getBeginLoc(), diag::note_acc_previous_clause_here);
    }

    return OpenACCSelfClause::Create(
        getASTContext(), Clause.getBeginLoc(), Clause.getLParenLoc(),
        Clause.getConditionExpr(), Clause.getEndLoc());
  }
  case OpenACCClauseKind::NumGangs: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // There is no prose in the standard that says duplicates aren't allowed,
    // but this diagnostic is present in other compilers, as well as makes
    // sense.
    if (checkAlreadyHasClauseOfKind(*this, ExistingClauses, Clause))
      return nullptr;

    if (Clause.getIntExprs().empty())
      Diag(Clause.getBeginLoc(), diag::err_acc_num_gangs_num_args)
          << /*NoArgs=*/0;

    unsigned MaxArgs =
        (Clause.getDirectiveKind() == OpenACCDirectiveKind::Parallel ||
         Clause.getDirectiveKind() == OpenACCDirectiveKind::ParallelLoop)
            ? 3
            : 1;
    if (Clause.getIntExprs().size() > MaxArgs)
      Diag(Clause.getBeginLoc(), diag::err_acc_num_gangs_num_args)
          << /*NoArgs=*/1 << Clause.getDirectiveKind() << MaxArgs
          << Clause.getIntExprs().size();

    // Create the AST node for the clause even if the number of expressions is
    // incorrect.
    return OpenACCNumGangsClause::Create(
        getASTContext(), Clause.getBeginLoc(), Clause.getLParenLoc(),
        Clause.getIntExprs(), Clause.getEndLoc());
    break;
  }
  case OpenACCClauseKind::NumWorkers: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // There is no prose in the standard that says duplicates aren't allowed,
    // but this diagnostic is present in other compilers, as well as makes
    // sense.
    if (checkAlreadyHasClauseOfKind(*this, ExistingClauses, Clause))
      return nullptr;

    assert(Clause.getIntExprs().size() == 1 &&
           "Invalid number of expressions for NumWorkers");
    return OpenACCNumWorkersClause::Create(
        getASTContext(), Clause.getBeginLoc(), Clause.getLParenLoc(),
        Clause.getIntExprs()[0], Clause.getEndLoc());
  }
  case OpenACCClauseKind::VectorLength: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // There is no prose in the standard that says duplicates aren't allowed,
    // but this diagnostic is present in other compilers, as well as makes
    // sense.
    if (checkAlreadyHasClauseOfKind(*this, ExistingClauses, Clause))
      return nullptr;

    assert(Clause.getIntExprs().size() == 1 &&
           "Invalid number of expressions for VectorLength");
    return OpenACCVectorLengthClause::Create(
        getASTContext(), Clause.getBeginLoc(), Clause.getLParenLoc(),
        Clause.getIntExprs()[0], Clause.getEndLoc());
  }
  case OpenACCClauseKind::Private: {
    // Restrictions only properly implemented on 'compute' constructs, and
    // 'compute' constructs are the only construct that can do anything with
    // this yet, so skip/treat as unimplemented in this case.
    if (!isOpenACCComputeDirectiveKind(Clause.getDirectiveKind()))
      break;

    // ActOnVar ensured that everything is a valid variable reference, so there
    // really isn't anything to do here. GCC does some duplicate-finding, though
    // it isn't apparent in the standard where this is justified.

    return OpenACCPrivateClause::Create(
        getASTContext(), Clause.getBeginLoc(), Clause.getLParenLoc(),
        Clause.getVarList(), Clause.getEndLoc());
  }
  default:
    break;
  }

  Diag(Clause.getBeginLoc(), diag::warn_acc_clause_unimplemented)
      << Clause.getClauseKind();
  return nullptr;
}

void SemaOpenACC::ActOnConstruct(OpenACCDirectiveKind K,
                                 SourceLocation StartLoc) {
  switch (K) {
  case OpenACCDirectiveKind::Invalid:
    // Nothing to do here, an invalid kind has nothing we can check here.  We
    // want to continue parsing clauses as far as we can, so we will just
    // ensure that we can still work and don't check any construct-specific
    // rules anywhere.
    break;
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::Kernels:
    // Nothing to do here, there is no real legalization that needs to happen
    // here as these constructs do not take any arguments.
    break;
  default:
    Diag(StartLoc, diag::warn_acc_construct_unimplemented) << K;
    break;
  }
}

ExprResult SemaOpenACC::ActOnIntExpr(OpenACCDirectiveKind DK,
                                     OpenACCClauseKind CK, SourceLocation Loc,
                                     Expr *IntExpr) {

  assert(((DK != OpenACCDirectiveKind::Invalid &&
           CK == OpenACCClauseKind::Invalid) ||
          (DK == OpenACCDirectiveKind::Invalid &&
           CK != OpenACCClauseKind::Invalid) ||
          (DK == OpenACCDirectiveKind::Invalid &&
           CK == OpenACCClauseKind::Invalid)) &&
         "Only one of directive or clause kind should be provided");

  class IntExprConverter : public Sema::ICEConvertDiagnoser {
    OpenACCDirectiveKind DirectiveKind;
    OpenACCClauseKind ClauseKind;
    Expr *IntExpr;

    // gets the index into the diagnostics so we can use this for clauses,
    // directives, and sub array.s
    unsigned getDiagKind() const {
      if (ClauseKind != OpenACCClauseKind::Invalid)
        return 0;
      if (DirectiveKind != OpenACCDirectiveKind::Invalid)
        return 1;
      return 2;
    }

  public:
    IntExprConverter(OpenACCDirectiveKind DK, OpenACCClauseKind CK,
                     Expr *IntExpr)
        : ICEConvertDiagnoser(/*AllowScopedEnumerations=*/false,
                              /*Suppress=*/false,
                              /*SuppressConversion=*/true),
          DirectiveKind(DK), ClauseKind(CK), IntExpr(IntExpr) {}

    bool match(QualType T) override {
      // OpenACC spec just calls this 'integer expression' as having an
      // 'integer type', so fall back on C99's 'integer type'.
      return T->isIntegerType();
    }
    SemaBase::SemaDiagnosticBuilder diagnoseNotInt(Sema &S, SourceLocation Loc,
                                                   QualType T) override {
      return S.Diag(Loc, diag::err_acc_int_expr_requires_integer)
             << getDiagKind() << ClauseKind << DirectiveKind << T;
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseIncomplete(Sema &S, SourceLocation Loc, QualType T) override {
      return S.Diag(Loc, diag::err_acc_int_expr_incomplete_class_type)
             << T << IntExpr->getSourceRange();
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseExplicitConv(Sema &S, SourceLocation Loc, QualType T,
                         QualType ConvTy) override {
      return S.Diag(Loc, diag::err_acc_int_expr_explicit_conversion)
             << T << ConvTy;
    }

    SemaBase::SemaDiagnosticBuilder noteExplicitConv(Sema &S,
                                                     CXXConversionDecl *Conv,
                                                     QualType ConvTy) override {
      return S.Diag(Conv->getLocation(), diag::note_acc_int_expr_conversion)
             << ConvTy->isEnumeralType() << ConvTy;
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseAmbiguous(Sema &S, SourceLocation Loc, QualType T) override {
      return S.Diag(Loc, diag::err_acc_int_expr_multiple_conversions) << T;
    }

    SemaBase::SemaDiagnosticBuilder
    noteAmbiguous(Sema &S, CXXConversionDecl *Conv, QualType ConvTy) override {
      return S.Diag(Conv->getLocation(), diag::note_acc_int_expr_conversion)
             << ConvTy->isEnumeralType() << ConvTy;
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseConversion(Sema &S, SourceLocation Loc, QualType T,
                       QualType ConvTy) override {
      llvm_unreachable("conversion functions are permitted");
    }
  } IntExprDiagnoser(DK, CK, IntExpr);

  ExprResult IntExprResult = SemaRef.PerformContextualImplicitConversion(
      Loc, IntExpr, IntExprDiagnoser);
  if (IntExprResult.isInvalid())
    return ExprError();

  IntExpr = IntExprResult.get();
  if (!IntExpr->isTypeDependent() && !IntExpr->getType()->isIntegerType())
    return ExprError();

  // TODO OpenACC: Do we want to perform usual unary conversions here? When
  // doing codegen we might find that is necessary, but skip it for now.
  return IntExpr;
}

ExprResult SemaOpenACC::ActOnVar(Expr *VarExpr) {
  // We still need to retain the array subscript/subarray exprs, so work on a
  // copy.
  Expr *CurVarExpr = VarExpr->IgnoreParenImpCasts();

  // Sub-arrays/subscript-exprs are fine as long as the base is a
  // VarExpr/MemberExpr. So strip all of those off.
  while (isa<ArraySectionExpr, ArraySubscriptExpr>(CurVarExpr)) {
    if (auto *SubScrpt = dyn_cast<ArraySubscriptExpr>(CurVarExpr))
      CurVarExpr = SubScrpt->getBase()->IgnoreParenImpCasts();
    else
      CurVarExpr =
          cast<ArraySectionExpr>(CurVarExpr)->getBase()->IgnoreParenImpCasts();
  }

  // References to a VarDecl are fine.
  if (const auto *DRE = dyn_cast<DeclRefExpr>(CurVarExpr)) {
    if (isa<VarDecl, NonTypeTemplateParmDecl>(
            DRE->getDecl()->getCanonicalDecl()))
      return VarExpr;
  }

  // A MemberExpr that references a Field is valid.
  if (const auto *ME = dyn_cast<MemberExpr>(CurVarExpr)) {
    if (isa<FieldDecl>(ME->getMemberDecl()->getCanonicalDecl()))
      return VarExpr;
  }

  // Referring to 'this' is always OK.
  if (isa<CXXThisExpr>(CurVarExpr))
    return VarExpr;

  // Nothing really we can do here, as these are dependent.  So just return they
  // are valid.
  if (isa<DependentScopeDeclRefExpr, CXXDependentScopeMemberExpr>(CurVarExpr))
    return VarExpr;

  // There isn't really anything we can do in the case of a recovery expr, so
  // skip the diagnostic rather than produce a confusing diagnostic.
  if (isa<RecoveryExpr>(CurVarExpr))
    return ExprError();

  Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref);
  return ExprError();
}

ExprResult SemaOpenACC::ActOnArraySectionExpr(Expr *Base, SourceLocation LBLoc,
                                              Expr *LowerBound,
                                              SourceLocation ColonLoc,
                                              Expr *Length,
                                              SourceLocation RBLoc) {
  ASTContext &Context = getASTContext();

  // Handle placeholders.
  if (Base->hasPlaceholderType() &&
      !Base->hasPlaceholderType(BuiltinType::ArraySection)) {
    ExprResult Result = SemaRef.CheckPlaceholderExpr(Base);
    if (Result.isInvalid())
      return ExprError();
    Base = Result.get();
  }
  if (LowerBound && LowerBound->getType()->isNonOverloadPlaceholderType()) {
    ExprResult Result = SemaRef.CheckPlaceholderExpr(LowerBound);
    if (Result.isInvalid())
      return ExprError();
    Result = SemaRef.DefaultLvalueConversion(Result.get());
    if (Result.isInvalid())
      return ExprError();
    LowerBound = Result.get();
  }
  if (Length && Length->getType()->isNonOverloadPlaceholderType()) {
    ExprResult Result = SemaRef.CheckPlaceholderExpr(Length);
    if (Result.isInvalid())
      return ExprError();
    Result = SemaRef.DefaultLvalueConversion(Result.get());
    if (Result.isInvalid())
      return ExprError();
    Length = Result.get();
  }

  // Check the 'base' value, it must be an array or pointer type, and not to/of
  // a function type.
  QualType OriginalBaseTy = ArraySectionExpr::getBaseOriginalType(Base);
  QualType ResultTy;
  if (!Base->isTypeDependent()) {
    if (OriginalBaseTy->isAnyPointerType()) {
      ResultTy = OriginalBaseTy->getPointeeType();
    } else if (OriginalBaseTy->isArrayType()) {
      ResultTy = OriginalBaseTy->getAsArrayTypeUnsafe()->getElementType();
    } else {
      return ExprError(
          Diag(Base->getExprLoc(), diag::err_acc_typecheck_subarray_value)
          << Base->getSourceRange());
    }

    if (ResultTy->isFunctionType()) {
      Diag(Base->getExprLoc(), diag::err_acc_subarray_function_type)
          << ResultTy << Base->getSourceRange();
      return ExprError();
    }

    if (SemaRef.RequireCompleteType(Base->getExprLoc(), ResultTy,
                                    diag::err_acc_subarray_incomplete_type,
                                    Base))
      return ExprError();

    if (!Base->hasPlaceholderType(BuiltinType::ArraySection)) {
      ExprResult Result = SemaRef.DefaultFunctionArrayLvalueConversion(Base);
      if (Result.isInvalid())
        return ExprError();
      Base = Result.get();
    }
  }

  auto GetRecovery = [&](Expr *E, QualType Ty) {
    ExprResult Recovery =
        SemaRef.CreateRecoveryExpr(E->getBeginLoc(), E->getEndLoc(), E, Ty);
    return Recovery.isUsable() ? Recovery.get() : nullptr;
  };

  // Ensure both of the expressions are int-exprs.
  if (LowerBound && !LowerBound->isTypeDependent()) {
    ExprResult LBRes =
        ActOnIntExpr(OpenACCDirectiveKind::Invalid, OpenACCClauseKind::Invalid,
                     LowerBound->getExprLoc(), LowerBound);

    if (LBRes.isUsable())
      LBRes = SemaRef.DefaultLvalueConversion(LBRes.get());
    LowerBound =
        LBRes.isUsable() ? LBRes.get() : GetRecovery(LowerBound, Context.IntTy);
  }

  if (Length && !Length->isTypeDependent()) {
    ExprResult LenRes =
        ActOnIntExpr(OpenACCDirectiveKind::Invalid, OpenACCClauseKind::Invalid,
                     Length->getExprLoc(), Length);

    if (LenRes.isUsable())
      LenRes = SemaRef.DefaultLvalueConversion(LenRes.get());
    Length =
        LenRes.isUsable() ? LenRes.get() : GetRecovery(Length, Context.IntTy);
  }

  // Length is required if the base type is not an array of known bounds.
  if (!Length && (OriginalBaseTy.isNull() ||
                  (!OriginalBaseTy->isDependentType() &&
                   !OriginalBaseTy->isConstantArrayType() &&
                   !OriginalBaseTy->isDependentSizedArrayType()))) {
    bool IsArray = !OriginalBaseTy.isNull() && OriginalBaseTy->isArrayType();
    Diag(ColonLoc, diag::err_acc_subarray_no_length) << IsArray;
    // Fill in a dummy 'length' so that when we instantiate this we don't
    // double-diagnose here.
    ExprResult Recovery = SemaRef.CreateRecoveryExpr(
        ColonLoc, SourceLocation(), ArrayRef<Expr *>{std::nullopt},
        Context.IntTy);
    Length = Recovery.isUsable() ? Recovery.get() : nullptr;
  }

  // Check the values of each of the arguments, they cannot be negative(we
  // assume), and if the array bound is known, must be within range. As we do
  // so, do our best to continue with evaluation, we can set the
  // value/expression to nullptr/nullopt if they are invalid, and treat them as
  // not present for the rest of evaluation.

  // We don't have to check for dependence, because the dependent size is
  // represented as a different AST node.
  std::optional<llvm::APSInt> BaseSize;
  if (!OriginalBaseTy.isNull() && OriginalBaseTy->isConstantArrayType()) {
    const auto *ArrayTy = Context.getAsConstantArrayType(OriginalBaseTy);
    BaseSize = ArrayTy->getSize();
  }

  auto GetBoundValue = [&](Expr *E) -> std::optional<llvm::APSInt> {
    if (!E || E->isInstantiationDependent())
      return std::nullopt;

    Expr::EvalResult Res;
    if (!E->EvaluateAsInt(Res, Context))
      return std::nullopt;
    return Res.Val.getInt();
  };

  std::optional<llvm::APSInt> LowerBoundValue = GetBoundValue(LowerBound);
  std::optional<llvm::APSInt> LengthValue = GetBoundValue(Length);

  // Check lower bound for negative or out of range.
  if (LowerBoundValue.has_value()) {
    if (LowerBoundValue->isNegative()) {
      Diag(LowerBound->getExprLoc(), diag::err_acc_subarray_negative)
          << /*LowerBound=*/0 << toString(*LowerBoundValue, /*Radix=*/10);
      LowerBoundValue.reset();
      LowerBound = GetRecovery(LowerBound, LowerBound->getType());
    } else if (BaseSize.has_value() &&
               llvm::APSInt::compareValues(*LowerBoundValue, *BaseSize) >= 0) {
      // Lower bound (start index) must be less than the size of the array.
      Diag(LowerBound->getExprLoc(), diag::err_acc_subarray_out_of_range)
          << /*LowerBound=*/0 << toString(*LowerBoundValue, /*Radix=*/10)
          << toString(*BaseSize, /*Radix=*/10);
      LowerBoundValue.reset();
      LowerBound = GetRecovery(LowerBound, LowerBound->getType());
    }
  }

  // Check length for negative or out of range.
  if (LengthValue.has_value()) {
    if (LengthValue->isNegative()) {
      Diag(Length->getExprLoc(), diag::err_acc_subarray_negative)
          << /*Length=*/1 << toString(*LengthValue, /*Radix=*/10);
      LengthValue.reset();
      Length = GetRecovery(Length, Length->getType());
    } else if (BaseSize.has_value() &&
               llvm::APSInt::compareValues(*LengthValue, *BaseSize) > 0) {
      // Length must be lessthan or EQUAL to the size of the array.
      Diag(Length->getExprLoc(), diag::err_acc_subarray_out_of_range)
          << /*Length=*/1 << toString(*LengthValue, /*Radix=*/10)
          << toString(*BaseSize, /*Radix=*/10);
      LengthValue.reset();
      Length = GetRecovery(Length, Length->getType());
    }
  }

  // Adding two APSInts requires matching sign, so extract that here.
  auto AddAPSInt = [](llvm::APSInt LHS, llvm::APSInt RHS) -> llvm::APSInt {
    if (LHS.isSigned() == RHS.isSigned())
      return LHS + RHS;

    unsigned Width = std::max(LHS.getBitWidth(), RHS.getBitWidth()) + 1;
    return llvm::APSInt(LHS.sext(Width) + RHS.sext(Width), /*Signed=*/true);
  };

  // If we know all 3 values, we can diagnose that the total value would be out
  // of range.
  if (BaseSize.has_value() && LowerBoundValue.has_value() &&
      LengthValue.has_value() &&
      llvm::APSInt::compareValues(AddAPSInt(*LowerBoundValue, *LengthValue),
                                  *BaseSize) > 0) {
    Diag(Base->getExprLoc(),
         diag::err_acc_subarray_base_plus_length_out_of_range)
        << toString(*LowerBoundValue, /*Radix=*/10)
        << toString(*LengthValue, /*Radix=*/10)
        << toString(*BaseSize, /*Radix=*/10);

    LowerBoundValue.reset();
    LowerBound = GetRecovery(LowerBound, LowerBound->getType());
    LengthValue.reset();
    Length = GetRecovery(Length, Length->getType());
  }

  // If any part of the expression is dependent, return a dependent sub-array.
  QualType ArrayExprTy = Context.ArraySectionTy;
  if (Base->isTypeDependent() ||
      (LowerBound && LowerBound->isInstantiationDependent()) ||
      (Length && Length->isInstantiationDependent()))
    ArrayExprTy = Context.DependentTy;

  return new (Context)
      ArraySectionExpr(Base, LowerBound, Length, ArrayExprTy, VK_LValue,
                       OK_Ordinary, ColonLoc, RBLoc);
}

bool SemaOpenACC::ActOnStartStmtDirective(OpenACCDirectiveKind K,
                                          SourceLocation StartLoc) {
  return diagnoseConstructAppertainment(*this, K, StartLoc, /*IsStmt=*/true);
}

StmtResult SemaOpenACC::ActOnEndStmtDirective(OpenACCDirectiveKind K,
                                              SourceLocation StartLoc,
                                              SourceLocation EndLoc,
                                              ArrayRef<OpenACCClause *> Clauses,
                                              StmtResult AssocStmt) {
  switch (K) {
  default:
    return StmtEmpty();
  case OpenACCDirectiveKind::Invalid:
    return StmtError();
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::Kernels:
    // TODO OpenACC: Add clauses to the construct here.
    return OpenACCComputeConstruct::Create(
        getASTContext(), K, StartLoc, EndLoc, Clauses,
        AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  llvm_unreachable("Unhandled case in directive handling?");
}

StmtResult SemaOpenACC::ActOnAssociatedStmt(OpenACCDirectiveKind K,
                                            StmtResult AssocStmt) {
  switch (K) {
  default:
    llvm_unreachable("Unimplemented associated statement application");
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::Kernels:
    // There really isn't any checking here that could happen. As long as we
    // have a statement to associate, this should be fine.
    // OpenACC 3.3 Section 6:
    // Structured Block: in C or C++, an executable statement, possibly
    // compound, with a single entry at the top and a single exit at the
    // bottom.
    // FIXME: Should we reject DeclStmt's here? The standard isn't clear, and
    // an interpretation of it is to allow this and treat the initializer as
    // the 'structured block'.
    return AssocStmt;
  }
  llvm_unreachable("Invalid associated statement application");
}

bool SemaOpenACC::ActOnStartDeclDirective(OpenACCDirectiveKind K,
                                          SourceLocation StartLoc) {
  return diagnoseConstructAppertainment(*this, K, StartLoc, /*IsStmt=*/false);
}

DeclGroupRef SemaOpenACC::ActOnEndDeclDirective() { return DeclGroupRef{}; }
