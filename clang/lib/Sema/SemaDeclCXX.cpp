//===------ SemaDeclCXX.cpp - Semantic Analysis for C++ Declarations ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for C++ declarations.
//
//===----------------------------------------------------------------------===//

#include "Sema.h"
#include "SemaInit.h"
#include "Lookup.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeOrdering.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Parse/DeclSpec.h"
#include "clang/Parse/Template.h"
#include "clang/Basic/PartialDiagnostic.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/STLExtras.h"
#include <map>
#include <set>

using namespace clang;

//===----------------------------------------------------------------------===//
// CheckDefaultArgumentVisitor
//===----------------------------------------------------------------------===//

namespace {
  /// CheckDefaultArgumentVisitor - C++ [dcl.fct.default] Traverses
  /// the default argument of a parameter to determine whether it
  /// contains any ill-formed subexpressions. For example, this will
  /// diagnose the use of local variables or parameters within the
  /// default argument expression.
  class CheckDefaultArgumentVisitor
    : public StmtVisitor<CheckDefaultArgumentVisitor, bool> {
    Expr *DefaultArg;
    Sema *S;

  public:
    CheckDefaultArgumentVisitor(Expr *defarg, Sema *s)
      : DefaultArg(defarg), S(s) {}

    bool VisitExpr(Expr *Node);
    bool VisitDeclRefExpr(DeclRefExpr *DRE);
    bool VisitCXXThisExpr(CXXThisExpr *ThisE);
  };

  /// VisitExpr - Visit all of the children of this expression.
  bool CheckDefaultArgumentVisitor::VisitExpr(Expr *Node) {
    bool IsInvalid = false;
    for (Stmt::child_iterator I = Node->child_begin(),
         E = Node->child_end(); I != E; ++I)
      IsInvalid |= Visit(*I);
    return IsInvalid;
  }

  /// VisitDeclRefExpr - Visit a reference to a declaration, to
  /// determine whether this declaration can be used in the default
  /// argument expression.
  bool CheckDefaultArgumentVisitor::VisitDeclRefExpr(DeclRefExpr *DRE) {
    NamedDecl *Decl = DRE->getDecl();
    if (ParmVarDecl *Param = dyn_cast<ParmVarDecl>(Decl)) {
      // C++ [dcl.fct.default]p9
      //   Default arguments are evaluated each time the function is
      //   called. The order of evaluation of function arguments is
      //   unspecified. Consequently, parameters of a function shall not
      //   be used in default argument expressions, even if they are not
      //   evaluated. Parameters of a function declared before a default
      //   argument expression are in scope and can hide namespace and
      //   class member names.
      return S->Diag(DRE->getSourceRange().getBegin(),
                     diag::err_param_default_argument_references_param)
         << Param->getDeclName() << DefaultArg->getSourceRange();
    } else if (VarDecl *VDecl = dyn_cast<VarDecl>(Decl)) {
      // C++ [dcl.fct.default]p7
      //   Local variables shall not be used in default argument
      //   expressions.
      if (VDecl->isBlockVarDecl())
        return S->Diag(DRE->getSourceRange().getBegin(),
                       diag::err_param_default_argument_references_local)
          << VDecl->getDeclName() << DefaultArg->getSourceRange();
    }

    return false;
  }

  /// VisitCXXThisExpr - Visit a C++ "this" expression.
  bool CheckDefaultArgumentVisitor::VisitCXXThisExpr(CXXThisExpr *ThisE) {
    // C++ [dcl.fct.default]p8:
    //   The keyword this shall not be used in a default argument of a
    //   member function.
    return S->Diag(ThisE->getSourceRange().getBegin(),
                   diag::err_param_default_argument_references_this)
               << ThisE->getSourceRange();
  }
}

bool
Sema::SetParamDefaultArgument(ParmVarDecl *Param, ExprArg DefaultArg,
                              SourceLocation EqualLoc) {
  if (RequireCompleteType(Param->getLocation(), Param->getType(),
                          diag::err_typecheck_decl_incomplete_type)) {
    Param->setInvalidDecl();
    return true;
  }

  Expr *Arg = (Expr *)DefaultArg.get();

  // C++ [dcl.fct.default]p5
  //   A default argument expression is implicitly converted (clause
  //   4) to the parameter type. The default argument expression has
  //   the same semantic constraints as the initializer expression in
  //   a declaration of a variable of the parameter type, using the
  //   copy-initialization semantics (8.5).
  InitializedEntity Entity = InitializedEntity::InitializeParameter(Param);
  InitializationKind Kind = InitializationKind::CreateCopy(Param->getLocation(),
                                                           EqualLoc);
  InitializationSequence InitSeq(*this, Entity, Kind, &Arg, 1);
  OwningExprResult Result = InitSeq.Perform(*this, Entity, Kind,
                                          MultiExprArg(*this, (void**)&Arg, 1));
  if (Result.isInvalid())
    return true;
  Arg = Result.takeAs<Expr>();

  Arg = MaybeCreateCXXExprWithTemporaries(Arg);

  // Okay: add the default argument to the parameter
  Param->setDefaultArg(Arg);

  DefaultArg.release();

  return false;
}

/// ActOnParamDefaultArgument - Check whether the default argument
/// provided for a function parameter is well-formed. If so, attach it
/// to the parameter declaration.
void
Sema::ActOnParamDefaultArgument(DeclPtrTy param, SourceLocation EqualLoc,
                                ExprArg defarg) {
  if (!param || !defarg.get())
    return;

  ParmVarDecl *Param = cast<ParmVarDecl>(param.getAs<Decl>());
  UnparsedDefaultArgLocs.erase(Param);

  ExprOwningPtr<Expr> DefaultArg(this, defarg.takeAs<Expr>());

  // Default arguments are only permitted in C++
  if (!getLangOptions().CPlusPlus) {
    Diag(EqualLoc, diag::err_param_default_argument)
      << DefaultArg->getSourceRange();
    Param->setInvalidDecl();
    return;
  }

  // Check that the default argument is well-formed
  CheckDefaultArgumentVisitor DefaultArgChecker(DefaultArg.get(), this);
  if (DefaultArgChecker.Visit(DefaultArg.get())) {
    Param->setInvalidDecl();
    return;
  }

  SetParamDefaultArgument(Param, move(DefaultArg), EqualLoc);
}

/// ActOnParamUnparsedDefaultArgument - We've seen a default
/// argument for a function parameter, but we can't parse it yet
/// because we're inside a class definition. Note that this default
/// argument will be parsed later.
void Sema::ActOnParamUnparsedDefaultArgument(DeclPtrTy param,
                                             SourceLocation EqualLoc,
                                             SourceLocation ArgLoc) {
  if (!param)
    return;

  ParmVarDecl *Param = cast<ParmVarDecl>(param.getAs<Decl>());
  if (Param)
    Param->setUnparsedDefaultArg();

  UnparsedDefaultArgLocs[Param] = ArgLoc;
}

/// ActOnParamDefaultArgumentError - Parsing or semantic analysis of
/// the default argument for the parameter param failed.
void Sema::ActOnParamDefaultArgumentError(DeclPtrTy param) {
  if (!param)
    return;

  ParmVarDecl *Param = cast<ParmVarDecl>(param.getAs<Decl>());

  Param->setInvalidDecl();

  UnparsedDefaultArgLocs.erase(Param);
}

/// CheckExtraCXXDefaultArguments - Check for any extra default
/// arguments in the declarator, which is not a function declaration
/// or definition and therefore is not permitted to have default
/// arguments. This routine should be invoked for every declarator
/// that is not a function declaration or definition.
void Sema::CheckExtraCXXDefaultArguments(Declarator &D) {
  // C++ [dcl.fct.default]p3
  //   A default argument expression shall be specified only in the
  //   parameter-declaration-clause of a function declaration or in a
  //   template-parameter (14.1). It shall not be specified for a
  //   parameter pack. If it is specified in a
  //   parameter-declaration-clause, it shall not occur within a
  //   declarator or abstract-declarator of a parameter-declaration.
  for (unsigned i = 0, e = D.getNumTypeObjects(); i != e; ++i) {
    DeclaratorChunk &chunk = D.getTypeObject(i);
    if (chunk.Kind == DeclaratorChunk::Function) {
      for (unsigned argIdx = 0, e = chunk.Fun.NumArgs; argIdx != e; ++argIdx) {
        ParmVarDecl *Param =
          cast<ParmVarDecl>(chunk.Fun.ArgInfo[argIdx].Param.getAs<Decl>());
        if (Param->hasUnparsedDefaultArg()) {
          CachedTokens *Toks = chunk.Fun.ArgInfo[argIdx].DefaultArgTokens;
          Diag(Param->getLocation(), diag::err_param_default_argument_nonfunc)
            << SourceRange((*Toks)[1].getLocation(), Toks->back().getLocation());
          delete Toks;
          chunk.Fun.ArgInfo[argIdx].DefaultArgTokens = 0;
        } else if (Param->getDefaultArg()) {
          Diag(Param->getLocation(), diag::err_param_default_argument_nonfunc)
            << Param->getDefaultArg()->getSourceRange();
          Param->setDefaultArg(0);
        }
      }
    }
  }
}

// MergeCXXFunctionDecl - Merge two declarations of the same C++
// function, once we already know that they have the same
// type. Subroutine of MergeFunctionDecl. Returns true if there was an
// error, false otherwise.
bool Sema::MergeCXXFunctionDecl(FunctionDecl *New, FunctionDecl *Old) {
  bool Invalid = false;

  // C++ [dcl.fct.default]p4:
  //   For non-template functions, default arguments can be added in
  //   later declarations of a function in the same
  //   scope. Declarations in different scopes have completely
  //   distinct sets of default arguments. That is, declarations in
  //   inner scopes do not acquire default arguments from
  //   declarations in outer scopes, and vice versa. In a given
  //   function declaration, all parameters subsequent to a
  //   parameter with a default argument shall have default
  //   arguments supplied in this or previous declarations. A
  //   default argument shall not be redefined by a later
  //   declaration (not even to the same value).
  //
  // C++ [dcl.fct.default]p6:
  //   Except for member functions of class templates, the default arguments 
  //   in a member function definition that appears outside of the class 
  //   definition are added to the set of default arguments provided by the 
  //   member function declaration in the class definition.
  for (unsigned p = 0, NumParams = Old->getNumParams(); p < NumParams; ++p) {
    ParmVarDecl *OldParam = Old->getParamDecl(p);
    ParmVarDecl *NewParam = New->getParamDecl(p);

    if (OldParam->hasDefaultArg() && NewParam->hasDefaultArg()) {
      // FIXME: If the parameter doesn't have an identifier then the location
      // points to the '=' which means that the fixit hint won't remove any
      // extra spaces between the type and the '='.
      SourceLocation Begin = NewParam->getLocation();
      if (NewParam->getIdentifier())
        Begin = PP.getLocForEndOfToken(Begin);
        
      Diag(NewParam->getLocation(),
           diag::err_param_default_argument_redefinition)
        << NewParam->getDefaultArgRange()
        << CodeModificationHint::CreateRemoval(SourceRange(Begin,
                                                        NewParam->getLocEnd()));
      
      // Look for the function declaration where the default argument was
      // actually written, which may be a declaration prior to Old.
      for (FunctionDecl *Older = Old->getPreviousDeclaration();
           Older; Older = Older->getPreviousDeclaration()) {
        if (!Older->getParamDecl(p)->hasDefaultArg())
          break;
        
        OldParam = Older->getParamDecl(p);
      }        
      
      Diag(OldParam->getLocation(), diag::note_previous_definition)
        << OldParam->getDefaultArgRange();
      Invalid = true;
    } else if (OldParam->hasDefaultArg()) {
      // Merge the old default argument into the new parameter
      if (OldParam->hasUninstantiatedDefaultArg())
        NewParam->setUninstantiatedDefaultArg(
                                      OldParam->getUninstantiatedDefaultArg());
      else
        NewParam->setDefaultArg(OldParam->getDefaultArg());
    } else if (NewParam->hasDefaultArg()) {
      if (New->getDescribedFunctionTemplate()) {
        // Paragraph 4, quoted above, only applies to non-template functions.
        Diag(NewParam->getLocation(),
             diag::err_param_default_argument_template_redecl)
          << NewParam->getDefaultArgRange();
        Diag(Old->getLocation(), diag::note_template_prev_declaration)
          << false;
      } else if (New->getTemplateSpecializationKind()
                   != TSK_ImplicitInstantiation &&
                 New->getTemplateSpecializationKind() != TSK_Undeclared) {
        // C++ [temp.expr.spec]p21:
        //   Default function arguments shall not be specified in a declaration
        //   or a definition for one of the following explicit specializations:
        //     - the explicit specialization of a function template;
        //     - the explicit specialization of a member function template;
        //     - the explicit specialization of a member function of a class 
        //       template where the class template specialization to which the
        //       member function specialization belongs is implicitly 
        //       instantiated.
        Diag(NewParam->getLocation(), diag::err_template_spec_default_arg)
          << (New->getTemplateSpecializationKind() ==TSK_ExplicitSpecialization)
          << New->getDeclName()
          << NewParam->getDefaultArgRange();
      } else if (New->getDeclContext()->isDependentContext()) {
        // C++ [dcl.fct.default]p6 (DR217):
        //   Default arguments for a member function of a class template shall 
        //   be specified on the initial declaration of the member function 
        //   within the class template.
        //
        // Reading the tea leaves a bit in DR217 and its reference to DR205 
        // leads me to the conclusion that one cannot add default function 
        // arguments for an out-of-line definition of a member function of a 
        // dependent type.
        int WhichKind = 2;
        if (CXXRecordDecl *Record 
              = dyn_cast<CXXRecordDecl>(New->getDeclContext())) {
          if (Record->getDescribedClassTemplate())
            WhichKind = 0;
          else if (isa<ClassTemplatePartialSpecializationDecl>(Record))
            WhichKind = 1;
          else
            WhichKind = 2;
        }
        
        Diag(NewParam->getLocation(), 
             diag::err_param_default_argument_member_template_redecl)
          << WhichKind
          << NewParam->getDefaultArgRange();
      }
    }
  }

  if (CheckEquivalentExceptionSpec(
          Old->getType()->getAs<FunctionProtoType>(), Old->getLocation(),
          New->getType()->getAs<FunctionProtoType>(), New->getLocation()))
    Invalid = true;

  return Invalid;
}

/// CheckCXXDefaultArguments - Verify that the default arguments for a
/// function declaration are well-formed according to C++
/// [dcl.fct.default].
void Sema::CheckCXXDefaultArguments(FunctionDecl *FD) {
  unsigned NumParams = FD->getNumParams();
  unsigned p;

  // Find first parameter with a default argument
  for (p = 0; p < NumParams; ++p) {
    ParmVarDecl *Param = FD->getParamDecl(p);
    if (Param->hasDefaultArg())
      break;
  }

  // C++ [dcl.fct.default]p4:
  //   In a given function declaration, all parameters
  //   subsequent to a parameter with a default argument shall
  //   have default arguments supplied in this or previous
  //   declarations. A default argument shall not be redefined
  //   by a later declaration (not even to the same value).
  unsigned LastMissingDefaultArg = 0;
  for (; p < NumParams; ++p) {
    ParmVarDecl *Param = FD->getParamDecl(p);
    if (!Param->hasDefaultArg()) {
      if (Param->isInvalidDecl())
        /* We already complained about this parameter. */;
      else if (Param->getIdentifier())
        Diag(Param->getLocation(),
             diag::err_param_default_argument_missing_name)
          << Param->getIdentifier();
      else
        Diag(Param->getLocation(),
             diag::err_param_default_argument_missing);

      LastMissingDefaultArg = p;
    }
  }

  if (LastMissingDefaultArg > 0) {
    // Some default arguments were missing. Clear out all of the
    // default arguments up to (and including) the last missing
    // default argument, so that we leave the function parameters
    // in a semantically valid state.
    for (p = 0; p <= LastMissingDefaultArg; ++p) {
      ParmVarDecl *Param = FD->getParamDecl(p);
      if (Param->hasDefaultArg()) {
        if (!Param->hasUnparsedDefaultArg())
          Param->getDefaultArg()->Destroy(Context);
        Param->setDefaultArg(0);
      }
    }
  }
}

/// isCurrentClassName - Determine whether the identifier II is the
/// name of the class type currently being defined. In the case of
/// nested classes, this will only return true if II is the name of
/// the innermost class.
bool Sema::isCurrentClassName(const IdentifierInfo &II, Scope *,
                              const CXXScopeSpec *SS) {
  CXXRecordDecl *CurDecl;
  if (SS && SS->isSet() && !SS->isInvalid()) {
    DeclContext *DC = computeDeclContext(*SS, true);
    CurDecl = dyn_cast_or_null<CXXRecordDecl>(DC);
  } else
    CurDecl = dyn_cast_or_null<CXXRecordDecl>(CurContext);

  if (CurDecl)
    return &II == CurDecl->getIdentifier();
  else
    return false;
}

/// \brief Check the validity of a C++ base class specifier.
///
/// \returns a new CXXBaseSpecifier if well-formed, emits diagnostics
/// and returns NULL otherwise.
CXXBaseSpecifier *
Sema::CheckBaseSpecifier(CXXRecordDecl *Class,
                         SourceRange SpecifierRange,
                         bool Virtual, AccessSpecifier Access,
                         QualType BaseType,
                         SourceLocation BaseLoc) {
  // C++ [class.union]p1:
  //   A union shall not have base classes.
  if (Class->isUnion()) {
    Diag(Class->getLocation(), diag::err_base_clause_on_union)
      << SpecifierRange;
    return 0;
  }

  if (BaseType->isDependentType())
    return new (Context) CXXBaseSpecifier(SpecifierRange, Virtual,
                                Class->getTagKind() == RecordDecl::TK_class,
                                Access, BaseType);

  // Base specifiers must be record types.
  if (!BaseType->isRecordType()) {
    Diag(BaseLoc, diag::err_base_must_be_class) << SpecifierRange;
    return 0;
  }

  // C++ [class.union]p1:
  //   A union shall not be used as a base class.
  if (BaseType->isUnionType()) {
    Diag(BaseLoc, diag::err_union_as_base_class) << SpecifierRange;
    return 0;
  }

  // C++ [class.derived]p2:
  //   The class-name in a base-specifier shall not be an incompletely
  //   defined class.
  if (RequireCompleteType(BaseLoc, BaseType,
                          PDiag(diag::err_incomplete_base_class)
                            << SpecifierRange))
    return 0;

  // If the base class is polymorphic or isn't empty, the new one is/isn't, too.
  RecordDecl *BaseDecl = BaseType->getAs<RecordType>()->getDecl();
  assert(BaseDecl && "Record type has no declaration");
  BaseDecl = BaseDecl->getDefinition(Context);
  assert(BaseDecl && "Base type is not incomplete, but has no definition");
  CXXRecordDecl * CXXBaseDecl = cast<CXXRecordDecl>(BaseDecl);
  assert(CXXBaseDecl && "Base type is not a C++ type");

  // C++0x CWG Issue #817 indicates that [[final]] classes shouldn't be bases.
  if (CXXBaseDecl->hasAttr<FinalAttr>()) {
    Diag(BaseLoc, diag::err_final_base) << BaseType.getAsString();
    Diag(CXXBaseDecl->getLocation(), diag::note_previous_decl)
      << BaseType;
    return 0;
  }

  SetClassDeclAttributesFromBase(Class, CXXBaseDecl, Virtual);
  
  // Create the base specifier.
  // FIXME: Allocate via ASTContext?
  return new (Context) CXXBaseSpecifier(SpecifierRange, Virtual,
                              Class->getTagKind() == RecordDecl::TK_class,
                              Access, BaseType);
}

void Sema::SetClassDeclAttributesFromBase(CXXRecordDecl *Class,
                                          const CXXRecordDecl *BaseClass,
                                          bool BaseIsVirtual) {
  // A class with a non-empty base class is not empty.
  // FIXME: Standard ref?
  if (!BaseClass->isEmpty())
    Class->setEmpty(false);

  // C++ [class.virtual]p1:
  //   A class that [...] inherits a virtual function is called a polymorphic
  //   class.
  if (BaseClass->isPolymorphic())
    Class->setPolymorphic(true);

  // C++ [dcl.init.aggr]p1:
  //   An aggregate is [...] a class with [...] no base classes [...].
  Class->setAggregate(false);

  // C++ [class]p4:
  //   A POD-struct is an aggregate class...
  Class->setPOD(false);

  if (BaseIsVirtual) {
    // C++ [class.ctor]p5:
    //   A constructor is trivial if its class has no virtual base classes.
    Class->setHasTrivialConstructor(false);

    // C++ [class.copy]p6:
    //   A copy constructor is trivial if its class has no virtual base classes.
    Class->setHasTrivialCopyConstructor(false);

    // C++ [class.copy]p11:
    //   A copy assignment operator is trivial if its class has no virtual
    //   base classes.
    Class->setHasTrivialCopyAssignment(false);

    // C++0x [meta.unary.prop] is_empty:
    //    T is a class type, but not a union type, with ... no virtual base
    //    classes
    Class->setEmpty(false);
  } else {
    // C++ [class.ctor]p5:
    //   A constructor is trivial if all the direct base classes of its
    //   class have trivial constructors.
    if (!BaseClass->hasTrivialConstructor())
      Class->setHasTrivialConstructor(false);

    // C++ [class.copy]p6:
    //   A copy constructor is trivial if all the direct base classes of its
    //   class have trivial copy constructors.
    if (!BaseClass->hasTrivialCopyConstructor())
      Class->setHasTrivialCopyConstructor(false);

    // C++ [class.copy]p11:
    //   A copy assignment operator is trivial if all the direct base classes
    //   of its class have trivial copy assignment operators.
    if (!BaseClass->hasTrivialCopyAssignment())
      Class->setHasTrivialCopyAssignment(false);
  }

  // C++ [class.ctor]p3:
  //   A destructor is trivial if all the direct base classes of its class
  //   have trivial destructors.
  if (!BaseClass->hasTrivialDestructor())
    Class->setHasTrivialDestructor(false);
}

/// ActOnBaseSpecifier - Parsed a base specifier. A base specifier is
/// one entry in the base class list of a class specifier, for
/// example:
///    class foo : public bar, virtual private baz {
/// 'public bar' and 'virtual private baz' are each base-specifiers.
Sema::BaseResult
Sema::ActOnBaseSpecifier(DeclPtrTy classdecl, SourceRange SpecifierRange,
                         bool Virtual, AccessSpecifier Access,
                         TypeTy *basetype, SourceLocation BaseLoc) {
  if (!classdecl)
    return true;

  AdjustDeclIfTemplate(classdecl);
  CXXRecordDecl *Class = cast<CXXRecordDecl>(classdecl.getAs<Decl>());
  QualType BaseType = GetTypeFromParser(basetype);
  if (CXXBaseSpecifier *BaseSpec = CheckBaseSpecifier(Class, SpecifierRange,
                                                      Virtual, Access,
                                                      BaseType, BaseLoc))
    return BaseSpec;

  return true;
}

/// \brief Performs the actual work of attaching the given base class
/// specifiers to a C++ class.
bool Sema::AttachBaseSpecifiers(CXXRecordDecl *Class, CXXBaseSpecifier **Bases,
                                unsigned NumBases) {
 if (NumBases == 0)
    return false;

  // Used to keep track of which base types we have already seen, so
  // that we can properly diagnose redundant direct base types. Note
  // that the key is always the unqualified canonical type of the base
  // class.
  std::map<QualType, CXXBaseSpecifier*, QualTypeOrdering> KnownBaseTypes;

  // Copy non-redundant base specifiers into permanent storage.
  unsigned NumGoodBases = 0;
  bool Invalid = false;
  for (unsigned idx = 0; idx < NumBases; ++idx) {
    QualType NewBaseType
      = Context.getCanonicalType(Bases[idx]->getType());
    NewBaseType = NewBaseType.getLocalUnqualifiedType();

    if (KnownBaseTypes[NewBaseType]) {
      // C++ [class.mi]p3:
      //   A class shall not be specified as a direct base class of a
      //   derived class more than once.
      Diag(Bases[idx]->getSourceRange().getBegin(),
           diag::err_duplicate_base_class)
        << KnownBaseTypes[NewBaseType]->getType()
        << Bases[idx]->getSourceRange();

      // Delete the duplicate base class specifier; we're going to
      // overwrite its pointer later.
      Context.Deallocate(Bases[idx]);

      Invalid = true;
    } else {
      // Okay, add this new base class.
      KnownBaseTypes[NewBaseType] = Bases[idx];
      Bases[NumGoodBases++] = Bases[idx];
    }
  }

  // Attach the remaining base class specifiers to the derived class.
  Class->setBases(Context, Bases, NumGoodBases);

  // Delete the remaining (good) base class specifiers, since their
  // data has been copied into the CXXRecordDecl.
  for (unsigned idx = 0; idx < NumGoodBases; ++idx)
    Context.Deallocate(Bases[idx]);

  return Invalid;
}

/// ActOnBaseSpecifiers - Attach the given base specifiers to the
/// class, after checking whether there are any duplicate base
/// classes.
void Sema::ActOnBaseSpecifiers(DeclPtrTy ClassDecl, BaseTy **Bases,
                               unsigned NumBases) {
  if (!ClassDecl || !Bases || !NumBases)
    return;

  AdjustDeclIfTemplate(ClassDecl);
  AttachBaseSpecifiers(cast<CXXRecordDecl>(ClassDecl.getAs<Decl>()),
                       (CXXBaseSpecifier**)(Bases), NumBases);
}

/// \brief Determine whether the type \p Derived is a C++ class that is
/// derived from the type \p Base.
bool Sema::IsDerivedFrom(QualType Derived, QualType Base) {
  if (!getLangOptions().CPlusPlus)
    return false;
    
  const RecordType *DerivedRT = Derived->getAs<RecordType>();
  if (!DerivedRT)
    return false;
  
  const RecordType *BaseRT = Base->getAs<RecordType>();
  if (!BaseRT)
    return false;
  
  CXXRecordDecl *DerivedRD = cast<CXXRecordDecl>(DerivedRT->getDecl());
  CXXRecordDecl *BaseRD = cast<CXXRecordDecl>(BaseRT->getDecl());
  return DerivedRD->isDerivedFrom(BaseRD);
}

/// \brief Determine whether the type \p Derived is a C++ class that is
/// derived from the type \p Base.
bool Sema::IsDerivedFrom(QualType Derived, QualType Base, CXXBasePaths &Paths) {
  if (!getLangOptions().CPlusPlus)
    return false;
  
  const RecordType *DerivedRT = Derived->getAs<RecordType>();
  if (!DerivedRT)
    return false;
  
  const RecordType *BaseRT = Base->getAs<RecordType>();
  if (!BaseRT)
    return false;
  
  CXXRecordDecl *DerivedRD = cast<CXXRecordDecl>(DerivedRT->getDecl());
  CXXRecordDecl *BaseRD = cast<CXXRecordDecl>(BaseRT->getDecl());
  return DerivedRD->isDerivedFrom(BaseRD, Paths);
}

/// CheckDerivedToBaseConversion - Check whether the Derived-to-Base
/// conversion (where Derived and Base are class types) is
/// well-formed, meaning that the conversion is unambiguous (and
/// that all of the base classes are accessible). Returns true
/// and emits a diagnostic if the code is ill-formed, returns false
/// otherwise. Loc is the location where this routine should point to
/// if there is an error, and Range is the source range to highlight
/// if there is an error.
bool
Sema::CheckDerivedToBaseConversion(QualType Derived, QualType Base,
                                   unsigned InaccessibleBaseID,
                                   unsigned AmbigiousBaseConvID,
                                   SourceLocation Loc, SourceRange Range,
                                   DeclarationName Name) {
  // First, determine whether the path from Derived to Base is
  // ambiguous. This is slightly more expensive than checking whether
  // the Derived to Base conversion exists, because here we need to
  // explore multiple paths to determine if there is an ambiguity.
  CXXBasePaths Paths(/*FindAmbiguities=*/true, /*RecordPaths=*/true,
                     /*DetectVirtual=*/false);
  bool DerivationOkay = IsDerivedFrom(Derived, Base, Paths);
  assert(DerivationOkay &&
         "Can only be used with a derived-to-base conversion");
  (void)DerivationOkay;
  
  if (!Paths.isAmbiguous(Context.getCanonicalType(Base).getUnqualifiedType())) {
    if (InaccessibleBaseID == 0)
      return false;
    // Check that the base class can be accessed.
    return CheckBaseClassAccess(Derived, Base, InaccessibleBaseID, Paths, Loc,
                                Name);
  }
  
  // We know that the derived-to-base conversion is ambiguous, and
  // we're going to produce a diagnostic. Perform the derived-to-base
  // search just one more time to compute all of the possible paths so
  // that we can print them out. This is more expensive than any of
  // the previous derived-to-base checks we've done, but at this point
  // performance isn't as much of an issue.
  Paths.clear();
  Paths.setRecordingPaths(true);
  bool StillOkay = IsDerivedFrom(Derived, Base, Paths);
  assert(StillOkay && "Can only be used with a derived-to-base conversion");
  (void)StillOkay;
  
  // Build up a textual representation of the ambiguous paths, e.g.,
  // D -> B -> A, that will be used to illustrate the ambiguous
  // conversions in the diagnostic. We only print one of the paths
  // to each base class subobject.
  std::string PathDisplayStr = getAmbiguousPathsDisplayString(Paths);
  
  Diag(Loc, AmbigiousBaseConvID)
  << Derived << Base << PathDisplayStr << Range << Name;
  return true;
}

bool
Sema::CheckDerivedToBaseConversion(QualType Derived, QualType Base,
                                   SourceLocation Loc, SourceRange Range,
                                   bool IgnoreAccess) {
  return CheckDerivedToBaseConversion(Derived, Base,
                                      IgnoreAccess ? 0 :
                                        diag::err_conv_to_inaccessible_base,
                                      diag::err_ambiguous_derived_to_base_conv,
                                      Loc, Range, DeclarationName());
}


/// @brief Builds a string representing ambiguous paths from a
/// specific derived class to different subobjects of the same base
/// class.
///
/// This function builds a string that can be used in error messages
/// to show the different paths that one can take through the
/// inheritance hierarchy to go from the derived class to different
/// subobjects of a base class. The result looks something like this:
/// @code
/// struct D -> struct B -> struct A
/// struct D -> struct C -> struct A
/// @endcode
std::string Sema::getAmbiguousPathsDisplayString(CXXBasePaths &Paths) {
  std::string PathDisplayStr;
  std::set<unsigned> DisplayedPaths;
  for (CXXBasePaths::paths_iterator Path = Paths.begin();
       Path != Paths.end(); ++Path) {
    if (DisplayedPaths.insert(Path->back().SubobjectNumber).second) {
      // We haven't displayed a path to this particular base
      // class subobject yet.
      PathDisplayStr += "\n    ";
      PathDisplayStr += Context.getTypeDeclType(Paths.getOrigin()).getAsString();
      for (CXXBasePath::const_iterator Element = Path->begin();
           Element != Path->end(); ++Element)
        PathDisplayStr += " -> " + Element->Base->getType().getAsString();
    }
  }
  
  return PathDisplayStr;
}

//===----------------------------------------------------------------------===//
// C++ class member Handling
//===----------------------------------------------------------------------===//

/// ActOnCXXMemberDeclarator - This is invoked when a C++ class member
/// declarator is parsed. 'AS' is the access specifier, 'BW' specifies the
/// bitfield width if there is one and 'InitExpr' specifies the initializer if
/// any.
Sema::DeclPtrTy
Sema::ActOnCXXMemberDeclarator(Scope *S, AccessSpecifier AS, Declarator &D,
                               MultiTemplateParamsArg TemplateParameterLists,
                               ExprTy *BW, ExprTy *InitExpr, bool IsDefinition,
                               bool Deleted) {
  const DeclSpec &DS = D.getDeclSpec();
  DeclarationName Name = GetNameForDeclarator(D);
  Expr *BitWidth = static_cast<Expr*>(BW);
  Expr *Init = static_cast<Expr*>(InitExpr);
  SourceLocation Loc = D.getIdentifierLoc();

  bool isFunc = D.isFunctionDeclarator();

  assert(!DS.isFriendSpecified());

  // C++ 9.2p6: A member shall not be declared to have automatic storage
  // duration (auto, register) or with the extern storage-class-specifier.
  // C++ 7.1.1p8: The mutable specifier can be applied only to names of class
  // data members and cannot be applied to names declared const or static,
  // and cannot be applied to reference members.
  switch (DS.getStorageClassSpec()) {
    case DeclSpec::SCS_unspecified:
    case DeclSpec::SCS_typedef:
    case DeclSpec::SCS_static:
      // FALL THROUGH.
      break;
    case DeclSpec::SCS_mutable:
      if (isFunc) {
        if (DS.getStorageClassSpecLoc().isValid())
          Diag(DS.getStorageClassSpecLoc(), diag::err_mutable_function);
        else
          Diag(DS.getThreadSpecLoc(), diag::err_mutable_function);

        // FIXME: It would be nicer if the keyword was ignored only for this
        // declarator. Otherwise we could get follow-up errors.
        D.getMutableDeclSpec().ClearStorageClassSpecs();
      } else {
        QualType T = GetTypeForDeclarator(D, S);
        diag::kind err = static_cast<diag::kind>(0);
        if (T->isReferenceType())
          err = diag::err_mutable_reference;
        else if (T.isConstQualified())
          err = diag::err_mutable_const;
        if (err != 0) {
          if (DS.getStorageClassSpecLoc().isValid())
            Diag(DS.getStorageClassSpecLoc(), err);
          else
            Diag(DS.getThreadSpecLoc(), err);
          // FIXME: It would be nicer if the keyword was ignored only for this
          // declarator. Otherwise we could get follow-up errors.
          D.getMutableDeclSpec().ClearStorageClassSpecs();
        }
      }
      break;
    default:
      if (DS.getStorageClassSpecLoc().isValid())
        Diag(DS.getStorageClassSpecLoc(),
             diag::err_storageclass_invalid_for_member);
      else
        Diag(DS.getThreadSpecLoc(), diag::err_storageclass_invalid_for_member);
      D.getMutableDeclSpec().ClearStorageClassSpecs();
  }

  if (!isFunc &&
      D.getDeclSpec().getTypeSpecType() == DeclSpec::TST_typename &&
      D.getNumTypeObjects() == 0) {
    // Check also for this case:
    //
    // typedef int f();
    // f a;
    //
    QualType TDType = GetTypeFromParser(DS.getTypeRep());
    isFunc = TDType->isFunctionType();
  }

  bool isInstField = ((DS.getStorageClassSpec() == DeclSpec::SCS_unspecified ||
                       DS.getStorageClassSpec() == DeclSpec::SCS_mutable) &&
                      !isFunc);

  Decl *Member;
  if (isInstField) {
    // FIXME: Check for template parameters!
    Member = HandleField(S, cast<CXXRecordDecl>(CurContext), Loc, D, BitWidth,
                         AS);
    assert(Member && "HandleField never returns null");
  } else {
    Member = HandleDeclarator(S, D, move(TemplateParameterLists), IsDefinition)
               .getAs<Decl>();
    if (!Member) {
      if (BitWidth) DeleteExpr(BitWidth);
      return DeclPtrTy();
    }

    // Non-instance-fields can't have a bitfield.
    if (BitWidth) {
      if (Member->isInvalidDecl()) {
        // don't emit another diagnostic.
      } else if (isa<VarDecl>(Member)) {
        // C++ 9.6p3: A bit-field shall not be a static member.
        // "static member 'A' cannot be a bit-field"
        Diag(Loc, diag::err_static_not_bitfield)
          << Name << BitWidth->getSourceRange();
      } else if (isa<TypedefDecl>(Member)) {
        // "typedef member 'x' cannot be a bit-field"
        Diag(Loc, diag::err_typedef_not_bitfield)
          << Name << BitWidth->getSourceRange();
      } else {
        // A function typedef ("typedef int f(); f a;").
        // C++ 9.6p3: A bit-field shall have integral or enumeration type.
        Diag(Loc, diag::err_not_integral_type_bitfield)
          << Name << cast<ValueDecl>(Member)->getType()
          << BitWidth->getSourceRange();
      }

      DeleteExpr(BitWidth);
      BitWidth = 0;
      Member->setInvalidDecl();
    }

    Member->setAccess(AS);

    // If we have declared a member function template, set the access of the
    // templated declaration as well.
    if (FunctionTemplateDecl *FunTmpl = dyn_cast<FunctionTemplateDecl>(Member))
      FunTmpl->getTemplatedDecl()->setAccess(AS);
  }

  assert((Name || isInstField) && "No identifier for non-field ?");

  if (Init)
    AddInitializerToDecl(DeclPtrTy::make(Member), ExprArg(*this, Init), false);
  if (Deleted) // FIXME: Source location is not very good.
    SetDeclDeleted(DeclPtrTy::make(Member), D.getSourceRange().getBegin());

  if (isInstField) {
    FieldCollector->Add(cast<FieldDecl>(Member));
    return DeclPtrTy();
  }
  return DeclPtrTy::make(Member);
}

/// ActOnMemInitializer - Handle a C++ member initializer.
Sema::MemInitResult
Sema::ActOnMemInitializer(DeclPtrTy ConstructorD,
                          Scope *S,
                          const CXXScopeSpec &SS,
                          IdentifierInfo *MemberOrBase,
                          TypeTy *TemplateTypeTy,
                          SourceLocation IdLoc,
                          SourceLocation LParenLoc,
                          ExprTy **Args, unsigned NumArgs,
                          SourceLocation *CommaLocs,
                          SourceLocation RParenLoc) {
  if (!ConstructorD)
    return true;

  AdjustDeclIfTemplate(ConstructorD);

  CXXConstructorDecl *Constructor
    = dyn_cast<CXXConstructorDecl>(ConstructorD.getAs<Decl>());
  if (!Constructor) {
    // The user wrote a constructor initializer on a function that is
    // not a C++ constructor. Ignore the error for now, because we may
    // have more member initializers coming; we'll diagnose it just
    // once in ActOnMemInitializers.
    return true;
  }

  CXXRecordDecl *ClassDecl = Constructor->getParent();

  // C++ [class.base.init]p2:
  //   Names in a mem-initializer-id are looked up in the scope of the
  //   constructor’s class and, if not found in that scope, are looked
  //   up in the scope containing the constructor’s
  //   definition. [Note: if the constructor’s class contains a member
  //   with the same name as a direct or virtual base class of the
  //   class, a mem-initializer-id naming the member or base class and
  //   composed of a single identifier refers to the class member. A
  //   mem-initializer-id for the hidden base class may be specified
  //   using a qualified name. ]
  if (!SS.getScopeRep() && !TemplateTypeTy) {
    // Look for a member, first.
    FieldDecl *Member = 0;
    DeclContext::lookup_result Result
      = ClassDecl->lookup(MemberOrBase);
    if (Result.first != Result.second)
      Member = dyn_cast<FieldDecl>(*Result.first);

    // FIXME: Handle members of an anonymous union.

    if (Member)
      return BuildMemberInitializer(Member, (Expr**)Args, NumArgs, IdLoc,
                                    LParenLoc, RParenLoc);
  }
  // It didn't name a member, so see if it names a class.
  QualType BaseType;
  TypeSourceInfo *TInfo = 0;

  if (TemplateTypeTy) {
    BaseType = GetTypeFromParser(TemplateTypeTy, &TInfo);
  } else {
    LookupResult R(*this, MemberOrBase, IdLoc, LookupOrdinaryName);
    LookupParsedName(R, S, &SS);

    TypeDecl *TyD = R.getAsSingle<TypeDecl>();
    if (!TyD) {
      if (R.isAmbiguous()) return true;

      Diag(IdLoc, diag::err_mem_init_not_member_or_class)
        << MemberOrBase << SourceRange(IdLoc, RParenLoc);
      return true;
    }

    BaseType = Context.getTypeDeclType(TyD);
    if (SS.isSet()) {
      NestedNameSpecifier *Qualifier =
        static_cast<NestedNameSpecifier*>(SS.getScopeRep());

      // FIXME: preserve source range information
      BaseType = Context.getQualifiedNameType(Qualifier, BaseType);
    }
  }

  if (!TInfo)
    TInfo = Context.getTrivialTypeSourceInfo(BaseType, IdLoc);

  return BuildBaseInitializer(BaseType, TInfo, (Expr **)Args, NumArgs, 
                              LParenLoc, RParenLoc, ClassDecl);
}

/// Checks an initializer expression for use of uninitialized fields, such as
/// containing the field that is being initialized. Returns true if there is an
/// uninitialized field was used an updates the SourceLocation parameter; false
/// otherwise.
static bool InitExprContainsUninitializedFields(const Stmt* S,
                                                const FieldDecl* LhsField,
                                                SourceLocation* L) {
  const MemberExpr* ME = dyn_cast<MemberExpr>(S);
  if (ME) {
    const NamedDecl* RhsField = ME->getMemberDecl();
    if (RhsField == LhsField) {
      // Initializing a field with itself. Throw a warning.
      // But wait; there are exceptions!
      // Exception #1:  The field may not belong to this record.
      // e.g. Foo(const Foo& rhs) : A(rhs.A) {}
      const Expr* base = ME->getBase();
      if (base != NULL && !isa<CXXThisExpr>(base->IgnoreParenCasts())) {
        // Even though the field matches, it does not belong to this record.
        return false;
      }
      // None of the exceptions triggered; return true to indicate an
      // uninitialized field was used.
      *L = ME->getMemberLoc();
      return true;
    }
  }
  bool found = false;
  for (Stmt::const_child_iterator it = S->child_begin();
       it != S->child_end() && found == false;
       ++it) {
    if (isa<CallExpr>(S)) {
      // Do not descend into function calls or constructors, as the use
      // of an uninitialized field may be valid. One would have to inspect
      // the contents of the function/ctor to determine if it is safe or not.
      // i.e. Pass-by-value is never safe, but pass-by-reference and pointers
      // may be safe, depending on what the function/ctor does.
      continue;
    }
    found = InitExprContainsUninitializedFields(*it, LhsField, L);
  }
  return found;
}

Sema::MemInitResult
Sema::BuildMemberInitializer(FieldDecl *Member, Expr **Args,
                             unsigned NumArgs, SourceLocation IdLoc,
                             SourceLocation LParenLoc,
                             SourceLocation RParenLoc) {
  // FIXME: CXXBaseOrMemberInitializer should only contain a single 
  // subexpression so we can wrap it in a CXXExprWithTemporaries if necessary.
  ExprTemporaries.clear();

  // Diagnose value-uses of fields to initialize themselves, e.g.
  //   foo(foo)
  // where foo is not also a parameter to the constructor.
  // TODO: implement -Wuninitialized and fold this into that framework.
  for (unsigned i = 0; i < NumArgs; ++i) {
    SourceLocation L;
    if (InitExprContainsUninitializedFields(Args[i], Member, &L)) {
      // FIXME: Return true in the case when other fields are used before being
      // uninitialized. For example, let this field be the i'th field. When
      // initializing the i'th field, throw a warning if any of the >= i'th
      // fields are used, as they are not yet initialized.
      // Right now we are only handling the case where the i'th field uses
      // itself in its initializer.
      Diag(L, diag::warn_field_is_uninit);
    }
  }

  bool HasDependentArg = false;
  for (unsigned i = 0; i < NumArgs; i++)
    HasDependentArg |= Args[i]->isTypeDependent();

  CXXConstructorDecl *C = 0;
  QualType FieldType = Member->getType();
  if (const ArrayType *Array = Context.getAsArrayType(FieldType))
    FieldType = Array->getElementType();
  ASTOwningVector<&ActionBase::DeleteExpr> ConstructorArgs(*this);
  if (FieldType->isDependentType()) {
    // Can't check init for dependent type.
  } else if (FieldType->isRecordType()) {
    // Member is a record (struct/union/class), so pass the initializer
    // arguments down to the record's constructor.
    if (!HasDependentArg) {
      C = PerformInitializationByConstructor(FieldType, 
                                             MultiExprArg(*this, 
                                                          (void**)Args, 
                                                          NumArgs), 
                                             IdLoc,
                                             SourceRange(IdLoc, RParenLoc), 
                                             Member->getDeclName(), 
                  InitializationKind::CreateDirect(IdLoc, LParenLoc, RParenLoc),
                                             ConstructorArgs);
      
      if (C) {
        // Take over the constructor arguments as our own.
        NumArgs = ConstructorArgs.size();
        Args = (Expr **)ConstructorArgs.take();
      }
    }
  } else if (NumArgs != 1 && NumArgs != 0) {
    // The member type is not a record type (or an array of record
    // types), so it can be only be default- or copy-initialized.
    return Diag(IdLoc, diag::err_mem_initializer_mismatch)
                << Member->getDeclName() << SourceRange(IdLoc, RParenLoc);
  } else if (!HasDependentArg) {
    Expr *NewExp;
    if (NumArgs == 0) {
      if (FieldType->isReferenceType()) {
        Diag(IdLoc, diag::err_null_intialized_reference_member)
              << Member->getDeclName();
        return Diag(Member->getLocation(), diag::note_declared_at);
      }
      NewExp = new (Context) CXXZeroInitValueExpr(FieldType, IdLoc, RParenLoc);
      NumArgs = 1;
    }
    else
      NewExp = (Expr*)Args[0];
    if (PerformCopyInitialization(NewExp, FieldType, AA_Passing))
      return true;
    Args[0] = NewExp;
  }
  
  // FIXME: CXXBaseOrMemberInitializer should only contain a single 
  // subexpression so we can wrap it in a CXXExprWithTemporaries if necessary.
  ExprTemporaries.clear();
  
  // FIXME: Perform direct initialization of the member.
  return new (Context) CXXBaseOrMemberInitializer(Context, Member, IdLoc,
                                                  C, LParenLoc, (Expr **)Args,
                                                  NumArgs, RParenLoc);
}

Sema::MemInitResult
Sema::BuildBaseInitializer(QualType BaseType, TypeSourceInfo *BaseTInfo,
                           Expr **Args, unsigned NumArgs, 
                           SourceLocation LParenLoc, SourceLocation RParenLoc, 
                           CXXRecordDecl *ClassDecl) {
  bool HasDependentArg = false;
  for (unsigned i = 0; i < NumArgs; i++)
    HasDependentArg |= Args[i]->isTypeDependent();

  SourceLocation BaseLoc = BaseTInfo->getTypeLoc().getSourceRange().getBegin();
  if (!BaseType->isDependentType()) {
    if (!BaseType->isRecordType())
      return Diag(BaseLoc, diag::err_base_init_does_not_name_class)
        << BaseType << BaseTInfo->getTypeLoc().getSourceRange();

    // C++ [class.base.init]p2:
    //   [...] Unless the mem-initializer-id names a nonstatic data
    //   member of the constructor’s class or a direct or virtual base
    //   of that class, the mem-initializer is ill-formed. A
    //   mem-initializer-list can initialize a base class using any
    //   name that denotes that base class type.

    // First, check for a direct base class.
    const CXXBaseSpecifier *DirectBaseSpec = 0;
    for (CXXRecordDecl::base_class_const_iterator Base =
         ClassDecl->bases_begin(); Base != ClassDecl->bases_end(); ++Base) {
      if (Context.hasSameUnqualifiedType(BaseType, Base->getType())) {
        // We found a direct base of this type. That's what we're
        // initializing.
        DirectBaseSpec = &*Base;
        break;
      }
    }

    // Check for a virtual base class.
    // FIXME: We might be able to short-circuit this if we know in advance that
    // there are no virtual bases.
    const CXXBaseSpecifier *VirtualBaseSpec = 0;
    if (!DirectBaseSpec || !DirectBaseSpec->isVirtual()) {
      // We haven't found a base yet; search the class hierarchy for a
      // virtual base class.
      CXXBasePaths Paths(/*FindAmbiguities=*/true, /*RecordPaths=*/true,
                         /*DetectVirtual=*/false);
      if (IsDerivedFrom(Context.getTypeDeclType(ClassDecl), BaseType, Paths)) {
        for (CXXBasePaths::paths_iterator Path = Paths.begin();
             Path != Paths.end(); ++Path) {
          if (Path->back().Base->isVirtual()) {
            VirtualBaseSpec = Path->back().Base;
            break;
          }
        }
      }
    }

    // C++ [base.class.init]p2:
    //   If a mem-initializer-id is ambiguous because it designates both
    //   a direct non-virtual base class and an inherited virtual base
    //   class, the mem-initializer is ill-formed.
    if (DirectBaseSpec && VirtualBaseSpec)
      return Diag(BaseLoc, diag::err_base_init_direct_and_virtual)
        << BaseType << BaseTInfo->getTypeLoc().getSourceRange();
    // C++ [base.class.init]p2:
    // Unless the mem-initializer-id names a nonstatic data membeer of the
    // constructor's class ot a direst or virtual base of that class, the
    // mem-initializer is ill-formed.
    if (!DirectBaseSpec && !VirtualBaseSpec)
      return Diag(BaseLoc, diag::err_not_direct_base_or_virtual)
        << BaseType << ClassDecl->getNameAsCString()
        << BaseTInfo->getTypeLoc().getSourceRange();
  }

  CXXConstructorDecl *C = 0;
  ASTOwningVector<&ActionBase::DeleteExpr> ConstructorArgs(*this);
  if (!BaseType->isDependentType() && !HasDependentArg) {
    DeclarationName Name = Context.DeclarationNames.getCXXConstructorName(
                      Context.getCanonicalType(BaseType).getUnqualifiedType());

    C = PerformInitializationByConstructor(BaseType, 
                                           MultiExprArg(*this, 
                                                        (void**)Args, NumArgs),
                                           BaseLoc, 
                                           SourceRange(BaseLoc, RParenLoc),
                                           Name, 
                InitializationKind::CreateDirect(BaseLoc, LParenLoc, RParenLoc),
                                           ConstructorArgs);
    if (C) {
      // Take over the constructor arguments as our own.
      NumArgs = ConstructorArgs.size();
      Args = (Expr **)ConstructorArgs.take();
    }
  }

  // FIXME: CXXBaseOrMemberInitializer should only contain a single 
  // subexpression so we can wrap it in a CXXExprWithTemporaries if necessary.
  ExprTemporaries.clear();
  
  return new (Context) CXXBaseOrMemberInitializer(Context, BaseTInfo, C, 
                                                  LParenLoc, (Expr **)Args, 
                                                  NumArgs, RParenLoc);
}

bool
Sema::SetBaseOrMemberInitializers(CXXConstructorDecl *Constructor,
                              CXXBaseOrMemberInitializer **Initializers,
                              unsigned NumInitializers,
                              bool IsImplicitConstructor) {
  // We need to build the initializer AST according to order of construction
  // and not what user specified in the Initializers list.
  CXXRecordDecl *ClassDecl = cast<CXXRecordDecl>(Constructor->getDeclContext());
  llvm::SmallVector<CXXBaseOrMemberInitializer*, 32> AllToInit;
  llvm::DenseMap<const void *, CXXBaseOrMemberInitializer*> AllBaseFields;
  bool HasDependentBaseInit = false;
  bool HadError = false;

  for (unsigned i = 0; i < NumInitializers; i++) {
    CXXBaseOrMemberInitializer *Member = Initializers[i];
    if (Member->isBaseInitializer()) {
      if (Member->getBaseClass()->isDependentType())
        HasDependentBaseInit = true;
      AllBaseFields[Member->getBaseClass()->getAs<RecordType>()] = Member;
    } else {
      AllBaseFields[Member->getMember()] = Member;
    }
  }

  if (HasDependentBaseInit) {
    // FIXME. This does not preserve the ordering of the initializers.
    // Try (with -Wreorder)
    // template<class X> struct A {};
    // template<class X> struct B : A<X> {
    //   B() : x1(10), A<X>() {}
    //   int x1;
    // };
    // B<int> x;
    // On seeing one dependent type, we should essentially exit this routine
    // while preserving user-declared initializer list. When this routine is
    // called during instantiatiation process, this routine will rebuild the
    // ordered initializer list correctly.

    // If we have a dependent base initialization, we can't determine the
    // association between initializers and bases; just dump the known
    // initializers into the list, and don't try to deal with other bases.
    for (unsigned i = 0; i < NumInitializers; i++) {
      CXXBaseOrMemberInitializer *Member = Initializers[i];
      if (Member->isBaseInitializer())
        AllToInit.push_back(Member);
    }
  } else {
    // Push virtual bases before others.
    for (CXXRecordDecl::base_class_iterator VBase =
         ClassDecl->vbases_begin(),
         E = ClassDecl->vbases_end(); VBase != E; ++VBase) {
      if (VBase->getType()->isDependentType())
        continue;
      if (CXXBaseOrMemberInitializer *Value
            = AllBaseFields.lookup(VBase->getType()->getAs<RecordType>())) {
        AllToInit.push_back(Value);
      }
      else {
        CXXRecordDecl *VBaseDecl =
          cast<CXXRecordDecl>(VBase->getType()->getAs<RecordType>()->getDecl());
        assert(VBaseDecl && "SetBaseOrMemberInitializers - VBaseDecl null");
        CXXConstructorDecl *Ctor = VBaseDecl->getDefaultConstructor(Context);
        if (!Ctor) {
          Diag(Constructor->getLocation(), diag::err_missing_default_ctor)
            << (int)IsImplicitConstructor << Context.getTagDeclType(ClassDecl)
            << 0 << VBase->getType();
          Diag(VBaseDecl->getLocation(), diag::note_previous_decl)
            << Context.getTagDeclType(VBaseDecl);
          HadError = true;
          continue;
        }

        ASTOwningVector<&ActionBase::DeleteExpr> CtorArgs(*this);
        if (CompleteConstructorCall(Ctor, MultiExprArg(*this, 0, 0), 
                                    Constructor->getLocation(), CtorArgs))
          continue;
        
        MarkDeclarationReferenced(Constructor->getLocation(), Ctor);
        
        // FIXME: CXXBaseOrMemberInitializer should only contain a single 
        // subexpression so we can wrap it in a CXXExprWithTemporaries if 
        // necessary.
        // FIXME: Is there any better source-location information we can give?
        ExprTemporaries.clear();
        CXXBaseOrMemberInitializer *Member =
          new (Context) CXXBaseOrMemberInitializer(Context,
                             Context.getTrivialTypeSourceInfo(VBase->getType(), 
                                                              SourceLocation()),
                                                   Ctor,
                                                   SourceLocation(),
                                                   CtorArgs.takeAs<Expr>(),
                                                   CtorArgs.size(), 
                                                   SourceLocation());
        AllToInit.push_back(Member);
      }
    }

    for (CXXRecordDecl::base_class_iterator Base =
         ClassDecl->bases_begin(),
         E = ClassDecl->bases_end(); Base != E; ++Base) {
      // Virtuals are in the virtual base list and already constructed.
      if (Base->isVirtual())
        continue;
      // Skip dependent types.
      if (Base->getType()->isDependentType())
        continue;
      if (CXXBaseOrMemberInitializer *Value
            = AllBaseFields.lookup(Base->getType()->getAs<RecordType>())) {
        AllToInit.push_back(Value);
      }
      else {
        CXXRecordDecl *BaseDecl =
          cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
        assert(BaseDecl && "SetBaseOrMemberInitializers - BaseDecl null");
         CXXConstructorDecl *Ctor = BaseDecl->getDefaultConstructor(Context);
        if (!Ctor) {
          Diag(Constructor->getLocation(), diag::err_missing_default_ctor)
            << (int)IsImplicitConstructor << Context.getTagDeclType(ClassDecl)
            << 0 << Base->getType();
          Diag(BaseDecl->getLocation(), diag::note_previous_decl)
            << Context.getTagDeclType(BaseDecl);
          HadError = true;
          continue;
        }

        ASTOwningVector<&ActionBase::DeleteExpr> CtorArgs(*this);
        if (CompleteConstructorCall(Ctor, MultiExprArg(*this, 0, 0), 
                                     Constructor->getLocation(), CtorArgs))
          continue;
        
        MarkDeclarationReferenced(Constructor->getLocation(), Ctor);

        // FIXME: CXXBaseOrMemberInitializer should only contain a single 
        // subexpression so we can wrap it in a CXXExprWithTemporaries if 
        // necessary.
        // FIXME: Is there any better source-location information we can give?
        ExprTemporaries.clear();
        CXXBaseOrMemberInitializer *Member =
          new (Context) CXXBaseOrMemberInitializer(Context,
                             Context.getTrivialTypeSourceInfo(Base->getType(), 
                                                              SourceLocation()),
                                                   Ctor,
                                                   SourceLocation(),
                                                   CtorArgs.takeAs<Expr>(),
                                                   CtorArgs.size(), 
                                                   SourceLocation());
        AllToInit.push_back(Member);
      }
    }
  }

  // non-static data members.
  for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin(),
       E = ClassDecl->field_end(); Field != E; ++Field) {
    if ((*Field)->isAnonymousStructOrUnion()) {
      if (const RecordType *FieldClassType =
          Field->getType()->getAs<RecordType>()) {
        CXXRecordDecl *FieldClassDecl
          = cast<CXXRecordDecl>(FieldClassType->getDecl());
        for (RecordDecl::field_iterator FA = FieldClassDecl->field_begin(),
            EA = FieldClassDecl->field_end(); FA != EA; FA++) {
          if (CXXBaseOrMemberInitializer *Value = AllBaseFields.lookup(*FA)) {
            // 'Member' is the anonymous union field and 'AnonUnionMember' is
            // set to the anonymous union data member used in the initializer
            // list.
            Value->setMember(*Field);
            Value->setAnonUnionMember(*FA);
            AllToInit.push_back(Value);
            break;
          }
        }
      }
      continue;
    }
    if (CXXBaseOrMemberInitializer *Value = AllBaseFields.lookup(*Field)) {
      AllToInit.push_back(Value);
      continue;
    }

    if ((*Field)->getType()->isDependentType())
      continue;
    
    QualType FT = Context.getBaseElementType((*Field)->getType());
    if (const RecordType* RT = FT->getAs<RecordType>()) {
      CXXConstructorDecl *Ctor =
        cast<CXXRecordDecl>(RT->getDecl())->getDefaultConstructor(Context);
      if (!Ctor) {
        Diag(Constructor->getLocation(), diag::err_missing_default_ctor)
          << (int)IsImplicitConstructor << Context.getTagDeclType(ClassDecl)
          << 1 << (*Field)->getDeclName();
        Diag(Field->getLocation(), diag::note_field_decl);
        Diag(RT->getDecl()->getLocation(), diag::note_previous_decl)
          << Context.getTagDeclType(RT->getDecl());
        HadError = true;
        continue;
      }

      if (FT.isConstQualified() && Ctor->isTrivial()) {
        Diag(Constructor->getLocation(), diag::err_unintialized_member_in_ctor)
          << (int)IsImplicitConstructor << Context.getTagDeclType(ClassDecl)
          << 1 << (*Field)->getDeclName();
        Diag((*Field)->getLocation(), diag::note_declared_at);
        HadError = true;
      }

      // Don't create initializers for trivial constructors, since they don't
      // actually need to be run.
      if (Ctor->isTrivial())
        continue;

      ASTOwningVector<&ActionBase::DeleteExpr> CtorArgs(*this);
      if (CompleteConstructorCall(Ctor, MultiExprArg(*this, 0, 0), 
                                  Constructor->getLocation(), CtorArgs))
        continue;
      
      // FIXME: CXXBaseOrMemberInitializer should only contain a single 
      // subexpression so we can wrap it in a CXXExprWithTemporaries if necessary.
      ExprTemporaries.clear();
      CXXBaseOrMemberInitializer *Member =
        new (Context) CXXBaseOrMemberInitializer(Context,
                                                 *Field, SourceLocation(),
                                                 Ctor,
                                                 SourceLocation(),
                                                 CtorArgs.takeAs<Expr>(),
                                                 CtorArgs.size(),
                                                 SourceLocation());

      AllToInit.push_back(Member);
      MarkDeclarationReferenced(Constructor->getLocation(), Ctor);
    }
    else if (FT->isReferenceType()) {
      Diag(Constructor->getLocation(), diag::err_unintialized_member_in_ctor)
        << (int)IsImplicitConstructor << Context.getTagDeclType(ClassDecl)
        << 0 << (*Field)->getDeclName();
      Diag((*Field)->getLocation(), diag::note_declared_at);
      HadError = true;
    }
    else if (FT.isConstQualified()) {
      Diag(Constructor->getLocation(), diag::err_unintialized_member_in_ctor)
        << (int)IsImplicitConstructor << Context.getTagDeclType(ClassDecl)
        << 1 << (*Field)->getDeclName();
      Diag((*Field)->getLocation(), diag::note_declared_at);
      HadError = true;
    }
  }

  NumInitializers = AllToInit.size();
  if (NumInitializers > 0) {
    Constructor->setNumBaseOrMemberInitializers(NumInitializers);
    CXXBaseOrMemberInitializer **baseOrMemberInitializers =
      new (Context) CXXBaseOrMemberInitializer*[NumInitializers];

    Constructor->setBaseOrMemberInitializers(baseOrMemberInitializers);
    for (unsigned Idx = 0; Idx < NumInitializers; ++Idx)
      baseOrMemberInitializers[Idx] = AllToInit[Idx];
  }

  return HadError;
}

static void *GetKeyForTopLevelField(FieldDecl *Field) {
  // For anonymous unions, use the class declaration as the key.
  if (const RecordType *RT = Field->getType()->getAs<RecordType>()) {
    if (RT->getDecl()->isAnonymousStructOrUnion())
      return static_cast<void *>(RT->getDecl());
  }
  return static_cast<void *>(Field);
}

static void *GetKeyForBase(QualType BaseType) {
  if (const RecordType *RT = BaseType->getAs<RecordType>())
    return (void *)RT;

  assert(0 && "Unexpected base type!");
  return 0;
}

static void *GetKeyForMember(CXXBaseOrMemberInitializer *Member,
                             bool MemberMaybeAnon = false) {
  // For fields injected into the class via declaration of an anonymous union,
  // use its anonymous union class declaration as the unique key.
  if (Member->isMemberInitializer()) {
    FieldDecl *Field = Member->getMember();

    // After SetBaseOrMemberInitializers call, Field is the anonymous union
    // data member of the class. Data member used in the initializer list is
    // in AnonUnionMember field.
    if (MemberMaybeAnon && Field->isAnonymousStructOrUnion())
      Field = Member->getAnonUnionMember();
    if (Field->getDeclContext()->isRecord()) {
      RecordDecl *RD = cast<RecordDecl>(Field->getDeclContext());
      if (RD->isAnonymousStructOrUnion())
        return static_cast<void *>(RD);
    }
    return static_cast<void *>(Field);
  }

  return GetKeyForBase(QualType(Member->getBaseClass(), 0));
}

/// ActOnMemInitializers - Handle the member initializers for a constructor.
void Sema::ActOnMemInitializers(DeclPtrTy ConstructorDecl,
                                SourceLocation ColonLoc,
                                MemInitTy **MemInits, unsigned NumMemInits) {
  if (!ConstructorDecl)
    return;

  AdjustDeclIfTemplate(ConstructorDecl);

  CXXConstructorDecl *Constructor
    = dyn_cast<CXXConstructorDecl>(ConstructorDecl.getAs<Decl>());

  if (!Constructor) {
    Diag(ColonLoc, diag::err_only_constructors_take_base_inits);
    return;
  }

  if (!Constructor->isDependentContext()) {
    llvm::DenseMap<void*, CXXBaseOrMemberInitializer *>Members;
    bool err = false;
    for (unsigned i = 0; i < NumMemInits; i++) {
      CXXBaseOrMemberInitializer *Member =
        static_cast<CXXBaseOrMemberInitializer*>(MemInits[i]);
      void *KeyToMember = GetKeyForMember(Member);
      CXXBaseOrMemberInitializer *&PrevMember = Members[KeyToMember];
      if (!PrevMember) {
        PrevMember = Member;
        continue;
      }
      if (FieldDecl *Field = Member->getMember())
        Diag(Member->getSourceLocation(),
             diag::error_multiple_mem_initialization)
          << Field->getNameAsString()
          << Member->getSourceRange();
      else {
        Type *BaseClass = Member->getBaseClass();
        assert(BaseClass && "ActOnMemInitializers - neither field or base");
        Diag(Member->getSourceLocation(),
             diag::error_multiple_base_initialization)
          << QualType(BaseClass, 0)
          << Member->getSourceRange();
      }
      Diag(PrevMember->getSourceLocation(), diag::note_previous_initializer)
        << 0;
      err = true;
    }

    if (err)
      return;
  }

  SetBaseOrMemberInitializers(Constructor,
                      reinterpret_cast<CXXBaseOrMemberInitializer **>(MemInits),
                      NumMemInits, false);

  if (Constructor->isDependentContext())
    return;

  if (Diags.getDiagnosticLevel(diag::warn_base_initialized) ==
      Diagnostic::Ignored &&
      Diags.getDiagnosticLevel(diag::warn_field_initialized) ==
      Diagnostic::Ignored)
    return;

  // Also issue warning if order of ctor-initializer list does not match order
  // of 1) base class declarations and 2) order of non-static data members.
  llvm::SmallVector<const void*, 32> AllBaseOrMembers;

  CXXRecordDecl *ClassDecl
    = cast<CXXRecordDecl>(Constructor->getDeclContext());
  // Push virtual bases before others.
  for (CXXRecordDecl::base_class_iterator VBase =
       ClassDecl->vbases_begin(),
       E = ClassDecl->vbases_end(); VBase != E; ++VBase)
    AllBaseOrMembers.push_back(GetKeyForBase(VBase->getType()));

  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin(),
       E = ClassDecl->bases_end(); Base != E; ++Base) {
    // Virtuals are alread in the virtual base list and are constructed
    // first.
    if (Base->isVirtual())
      continue;
    AllBaseOrMembers.push_back(GetKeyForBase(Base->getType()));
  }

  for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin(),
       E = ClassDecl->field_end(); Field != E; ++Field)
    AllBaseOrMembers.push_back(GetKeyForTopLevelField(*Field));

  int Last = AllBaseOrMembers.size();
  int curIndex = 0;
  CXXBaseOrMemberInitializer *PrevMember = 0;
  for (unsigned i = 0; i < NumMemInits; i++) {
    CXXBaseOrMemberInitializer *Member =
      static_cast<CXXBaseOrMemberInitializer*>(MemInits[i]);
    void *MemberInCtorList = GetKeyForMember(Member, true);

    for (; curIndex < Last; curIndex++)
      if (MemberInCtorList == AllBaseOrMembers[curIndex])
        break;
    if (curIndex == Last) {
      assert(PrevMember && "Member not in member list?!");
      // Initializer as specified in ctor-initializer list is out of order.
      // Issue a warning diagnostic.
      if (PrevMember->isBaseInitializer()) {
        // Diagnostics is for an initialized base class.
        Type *BaseClass = PrevMember->getBaseClass();
        Diag(PrevMember->getSourceLocation(),
             diag::warn_base_initialized)
          << QualType(BaseClass, 0);
      } else {
        FieldDecl *Field = PrevMember->getMember();
        Diag(PrevMember->getSourceLocation(),
             diag::warn_field_initialized)
          << Field->getNameAsString();
      }
      // Also the note!
      if (FieldDecl *Field = Member->getMember())
        Diag(Member->getSourceLocation(),
             diag::note_fieldorbase_initialized_here) << 0
          << Field->getNameAsString();
      else {
        Type *BaseClass = Member->getBaseClass();
        Diag(Member->getSourceLocation(),
             diag::note_fieldorbase_initialized_here) << 1
          << QualType(BaseClass, 0);
      }
      for (curIndex = 0; curIndex < Last; curIndex++)
        if (MemberInCtorList == AllBaseOrMembers[curIndex])
          break;
    }
    PrevMember = Member;
  }
}

void
Sema::MarkBaseAndMemberDestructorsReferenced(CXXDestructorDecl *Destructor) {
  // Ignore dependent destructors.
  if (Destructor->isDependentContext())
    return;
  
  CXXRecordDecl *ClassDecl = Destructor->getParent();

  // Non-static data members.
  for (CXXRecordDecl::field_iterator I = ClassDecl->field_begin(),
       E = ClassDecl->field_end(); I != E; ++I) {
    FieldDecl *Field = *I;
    
    QualType FieldType = Context.getBaseElementType(Field->getType());
    
    const RecordType* RT = FieldType->getAs<RecordType>();
    if (!RT)
      continue;
    
    CXXRecordDecl *FieldClassDecl = cast<CXXRecordDecl>(RT->getDecl());
    if (FieldClassDecl->hasTrivialDestructor())
      continue;

    const CXXDestructorDecl *Dtor = FieldClassDecl->getDestructor(Context);
    MarkDeclarationReferenced(Destructor->getLocation(),
                              const_cast<CXXDestructorDecl*>(Dtor));
  }

  // Bases.
  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin(),
       E = ClassDecl->bases_end(); Base != E; ++Base) {
    // Ignore virtual bases.
    if (Base->isVirtual())
      continue;

    // Ignore trivial destructors.
    CXXRecordDecl *BaseClassDecl
      = cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
    if (BaseClassDecl->hasTrivialDestructor())
      continue;
    
    const CXXDestructorDecl *Dtor = BaseClassDecl->getDestructor(Context);
    MarkDeclarationReferenced(Destructor->getLocation(),
                              const_cast<CXXDestructorDecl*>(Dtor));
  }
  
  // Virtual bases.
  for (CXXRecordDecl::base_class_iterator VBase = ClassDecl->vbases_begin(),
       E = ClassDecl->vbases_end(); VBase != E; ++VBase) {
    // Ignore trivial destructors.
    CXXRecordDecl *BaseClassDecl
      = cast<CXXRecordDecl>(VBase->getType()->getAs<RecordType>()->getDecl());
    if (BaseClassDecl->hasTrivialDestructor())
      continue;
    
    const CXXDestructorDecl *Dtor = BaseClassDecl->getDestructor(Context);
    MarkDeclarationReferenced(Destructor->getLocation(),
                              const_cast<CXXDestructorDecl*>(Dtor));
  }
}

void Sema::ActOnDefaultCtorInitializers(DeclPtrTy CDtorDecl) {
  if (!CDtorDecl)
    return;

  AdjustDeclIfTemplate(CDtorDecl);

  if (CXXConstructorDecl *Constructor
      = dyn_cast<CXXConstructorDecl>(CDtorDecl.getAs<Decl>()))
    SetBaseOrMemberInitializers(Constructor, 0, 0, false);
}

namespace {
  /// PureVirtualMethodCollector - traverses a class and its superclasses
  /// and determines if it has any pure virtual methods.
  class PureVirtualMethodCollector {
    ASTContext &Context;

  public:
    typedef llvm::SmallVector<const CXXMethodDecl*, 8> MethodList;

  private:
    MethodList Methods;

    void Collect(const CXXRecordDecl* RD, MethodList& Methods);

  public:
    PureVirtualMethodCollector(ASTContext &Ctx, const CXXRecordDecl* RD)
      : Context(Ctx) {

      MethodList List;
      Collect(RD, List);

      // Copy the temporary list to methods, and make sure to ignore any
      // null entries.
      for (size_t i = 0, e = List.size(); i != e; ++i) {
        if (List[i])
          Methods.push_back(List[i]);
      }
    }

    bool empty() const { return Methods.empty(); }

    MethodList::const_iterator methods_begin() { return Methods.begin(); }
    MethodList::const_iterator methods_end() { return Methods.end(); }
  };

  void PureVirtualMethodCollector::Collect(const CXXRecordDecl* RD,
                                           MethodList& Methods) {
    // First, collect the pure virtual methods for the base classes.
    for (CXXRecordDecl::base_class_const_iterator Base = RD->bases_begin(),
         BaseEnd = RD->bases_end(); Base != BaseEnd; ++Base) {
      if (const RecordType *RT = Base->getType()->getAs<RecordType>()) {
        const CXXRecordDecl *BaseDecl = cast<CXXRecordDecl>(RT->getDecl());
        if (BaseDecl && BaseDecl->isAbstract())
          Collect(BaseDecl, Methods);
      }
    }

    // Next, zero out any pure virtual methods that this class overrides.
    typedef llvm::SmallPtrSet<const CXXMethodDecl*, 4> MethodSetTy;

    MethodSetTy OverriddenMethods;
    size_t MethodsSize = Methods.size();

    for (RecordDecl::decl_iterator i = RD->decls_begin(), e = RD->decls_end();
         i != e; ++i) {
      // Traverse the record, looking for methods.
      if (CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(*i)) {
        // If the method is pure virtual, add it to the methods vector.
        if (MD->isPure())
          Methods.push_back(MD);

        // Record all the overridden methods in our set.
        for (CXXMethodDecl::method_iterator I = MD->begin_overridden_methods(),
             E = MD->end_overridden_methods(); I != E; ++I) {
          // Keep track of the overridden methods.
          OverriddenMethods.insert(*I);
        }
      }
    }

    // Now go through the methods and zero out all the ones we know are
    // overridden.
    for (size_t i = 0, e = MethodsSize; i != e; ++i) {
      if (OverriddenMethods.count(Methods[i]))
        Methods[i] = 0;
    }

  }
}


bool Sema::RequireNonAbstractType(SourceLocation Loc, QualType T,
                                  unsigned DiagID, AbstractDiagSelID SelID,
                                  const CXXRecordDecl *CurrentRD) {
  if (SelID == -1)
    return RequireNonAbstractType(Loc, T,
                                  PDiag(DiagID), CurrentRD);
  else
    return RequireNonAbstractType(Loc, T,
                                  PDiag(DiagID) << SelID, CurrentRD);
}

bool Sema::RequireNonAbstractType(SourceLocation Loc, QualType T,
                                  const PartialDiagnostic &PD,
                                  const CXXRecordDecl *CurrentRD) {
  if (!getLangOptions().CPlusPlus)
    return false;

  if (const ArrayType *AT = Context.getAsArrayType(T))
    return RequireNonAbstractType(Loc, AT->getElementType(), PD,
                                  CurrentRD);

  if (const PointerType *PT = T->getAs<PointerType>()) {
    // Find the innermost pointer type.
    while (const PointerType *T = PT->getPointeeType()->getAs<PointerType>())
      PT = T;

    if (const ArrayType *AT = Context.getAsArrayType(PT->getPointeeType()))
      return RequireNonAbstractType(Loc, AT->getElementType(), PD, CurrentRD);
  }

  const RecordType *RT = T->getAs<RecordType>();
  if (!RT)
    return false;

  const CXXRecordDecl *RD = dyn_cast<CXXRecordDecl>(RT->getDecl());
  if (!RD)
    return false;

  if (CurrentRD && CurrentRD != RD)
    return false;

  if (!RD->isAbstract())
    return false;

  Diag(Loc, PD) << RD->getDeclName();

  // Check if we've already emitted the list of pure virtual functions for this
  // class.
  if (PureVirtualClassDiagSet && PureVirtualClassDiagSet->count(RD))
    return true;

  PureVirtualMethodCollector Collector(Context, RD);

  for (PureVirtualMethodCollector::MethodList::const_iterator I =
       Collector.methods_begin(), E = Collector.methods_end(); I != E; ++I) {
    const CXXMethodDecl *MD = *I;

    Diag(MD->getLocation(), diag::note_pure_virtual_function) <<
      MD->getDeclName();
  }

  if (!PureVirtualClassDiagSet)
    PureVirtualClassDiagSet.reset(new RecordDeclSetTy);
  PureVirtualClassDiagSet->insert(RD);

  return true;
}

namespace {
  class AbstractClassUsageDiagnoser
    : public DeclVisitor<AbstractClassUsageDiagnoser, bool> {
    Sema &SemaRef;
    CXXRecordDecl *AbstractClass;

    bool VisitDeclContext(const DeclContext *DC) {
      bool Invalid = false;

      for (CXXRecordDecl::decl_iterator I = DC->decls_begin(),
           E = DC->decls_end(); I != E; ++I)
        Invalid |= Visit(*I);

      return Invalid;
    }

  public:
    AbstractClassUsageDiagnoser(Sema& SemaRef, CXXRecordDecl *ac)
      : SemaRef(SemaRef), AbstractClass(ac) {
        Visit(SemaRef.Context.getTranslationUnitDecl());
    }

    bool VisitFunctionDecl(const FunctionDecl *FD) {
      if (FD->isThisDeclarationADefinition()) {
        // No need to do the check if we're in a definition, because it requires
        // that the return/param types are complete.
        // because that requires
        return VisitDeclContext(FD);
      }

      // Check the return type.
      QualType RTy = FD->getType()->getAs<FunctionType>()->getResultType();
      bool Invalid =
        SemaRef.RequireNonAbstractType(FD->getLocation(), RTy,
                                       diag::err_abstract_type_in_decl,
                                       Sema::AbstractReturnType,
                                       AbstractClass);

      for (FunctionDecl::param_const_iterator I = FD->param_begin(),
           E = FD->param_end(); I != E; ++I) {
        const ParmVarDecl *VD = *I;
        Invalid |=
          SemaRef.RequireNonAbstractType(VD->getLocation(),
                                         VD->getOriginalType(),
                                         diag::err_abstract_type_in_decl,
                                         Sema::AbstractParamType,
                                         AbstractClass);
      }

      return Invalid;
    }

    bool VisitDecl(const Decl* D) {
      if (const DeclContext *DC = dyn_cast<DeclContext>(D))
        return VisitDeclContext(DC);

      return false;
    }
  };
}

/// \brief Perform semantic checks on a class definition that has been
/// completing, introducing implicitly-declared members, checking for
/// abstract types, etc.
void Sema::CheckCompletedCXXClass(CXXRecordDecl *Record) {
  if (!Record || Record->isInvalidDecl())
    return;

  if (!Record->isDependentType())
    AddImplicitlyDeclaredMembersToClass(Record);

  if (Record->isInvalidDecl())
    return;

  if (!Record->isAbstract()) {
    // Collect all the pure virtual methods and see if this is an abstract
    // class after all.
    PureVirtualMethodCollector Collector(Context, Record);
    if (!Collector.empty())
      Record->setAbstract(true);
  }

  if (Record->isAbstract())
    (void)AbstractClassUsageDiagnoser(*this, Record);
}

void Sema::ActOnFinishCXXMemberSpecification(Scope* S, SourceLocation RLoc,
                                             DeclPtrTy TagDecl,
                                             SourceLocation LBrac,
                                             SourceLocation RBrac) {
  if (!TagDecl)
    return;

  AdjustDeclIfTemplate(TagDecl);

  ActOnFields(S, RLoc, TagDecl,
              (DeclPtrTy*)FieldCollector->getCurFields(),
              FieldCollector->getCurNumFields(), LBrac, RBrac, 0);

  CheckCompletedCXXClass(
                      dyn_cast_or_null<CXXRecordDecl>(TagDecl.getAs<Decl>()));
}

/// AddImplicitlyDeclaredMembersToClass - Adds any implicitly-declared
/// special functions, such as the default constructor, copy
/// constructor, or destructor, to the given C++ class (C++
/// [special]p1).  This routine can only be executed just before the
/// definition of the class is complete.
void Sema::AddImplicitlyDeclaredMembersToClass(CXXRecordDecl *ClassDecl) {
  CanQualType ClassType
    = Context.getCanonicalType(Context.getTypeDeclType(ClassDecl));

  // FIXME: Implicit declarations have exception specifications, which are
  // the union of the specifications of the implicitly called functions.

  if (!ClassDecl->hasUserDeclaredConstructor()) {
    // C++ [class.ctor]p5:
    //   A default constructor for a class X is a constructor of class X
    //   that can be called without an argument. If there is no
    //   user-declared constructor for class X, a default constructor is
    //   implicitly declared. An implicitly-declared default constructor
    //   is an inline public member of its class.
    DeclarationName Name
      = Context.DeclarationNames.getCXXConstructorName(ClassType);
    CXXConstructorDecl *DefaultCon =
      CXXConstructorDecl::Create(Context, ClassDecl,
                                 ClassDecl->getLocation(), Name,
                                 Context.getFunctionType(Context.VoidTy,
                                                         0, 0, false, 0),
                                 /*TInfo=*/0,
                                 /*isExplicit=*/false,
                                 /*isInline=*/true,
                                 /*isImplicitlyDeclared=*/true);
    DefaultCon->setAccess(AS_public);
    DefaultCon->setImplicit();
    DefaultCon->setTrivial(ClassDecl->hasTrivialConstructor());
    ClassDecl->addDecl(DefaultCon);
  }

  if (!ClassDecl->hasUserDeclaredCopyConstructor()) {
    // C++ [class.copy]p4:
    //   If the class definition does not explicitly declare a copy
    //   constructor, one is declared implicitly.

    // C++ [class.copy]p5:
    //   The implicitly-declared copy constructor for a class X will
    //   have the form
    //
    //       X::X(const X&)
    //
    //   if
    bool HasConstCopyConstructor = true;

    //     -- each direct or virtual base class B of X has a copy
    //        constructor whose first parameter is of type const B& or
    //        const volatile B&, and
    for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin();
         HasConstCopyConstructor && Base != ClassDecl->bases_end(); ++Base) {
      const CXXRecordDecl *BaseClassDecl
        = cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
      HasConstCopyConstructor
        = BaseClassDecl->hasConstCopyConstructor(Context);
    }

    //     -- for all the nonstatic data members of X that are of a
    //        class type M (or array thereof), each such class type
    //        has a copy constructor whose first parameter is of type
    //        const M& or const volatile M&.
    for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin();
         HasConstCopyConstructor && Field != ClassDecl->field_end();
         ++Field) {
      QualType FieldType = (*Field)->getType();
      if (const ArrayType *Array = Context.getAsArrayType(FieldType))
        FieldType = Array->getElementType();
      if (const RecordType *FieldClassType = FieldType->getAs<RecordType>()) {
        const CXXRecordDecl *FieldClassDecl
          = cast<CXXRecordDecl>(FieldClassType->getDecl());
        HasConstCopyConstructor
          = FieldClassDecl->hasConstCopyConstructor(Context);
      }
    }

    //   Otherwise, the implicitly declared copy constructor will have
    //   the form
    //
    //       X::X(X&)
    QualType ArgType = ClassType;
    if (HasConstCopyConstructor)
      ArgType = ArgType.withConst();
    ArgType = Context.getLValueReferenceType(ArgType);

    //   An implicitly-declared copy constructor is an inline public
    //   member of its class.
    DeclarationName Name
      = Context.DeclarationNames.getCXXConstructorName(ClassType);
    CXXConstructorDecl *CopyConstructor
      = CXXConstructorDecl::Create(Context, ClassDecl,
                                   ClassDecl->getLocation(), Name,
                                   Context.getFunctionType(Context.VoidTy,
                                                           &ArgType, 1,
                                                           false, 0),
                                   /*TInfo=*/0,
                                   /*isExplicit=*/false,
                                   /*isInline=*/true,
                                   /*isImplicitlyDeclared=*/true);
    CopyConstructor->setAccess(AS_public);
    CopyConstructor->setImplicit();
    CopyConstructor->setTrivial(ClassDecl->hasTrivialCopyConstructor());

    // Add the parameter to the constructor.
    ParmVarDecl *FromParam = ParmVarDecl::Create(Context, CopyConstructor,
                                                 ClassDecl->getLocation(),
                                                 /*IdentifierInfo=*/0,
                                                 ArgType, /*TInfo=*/0,
                                                 VarDecl::None, 0);
    CopyConstructor->setParams(Context, &FromParam, 1);
    ClassDecl->addDecl(CopyConstructor);
  }

  if (!ClassDecl->hasUserDeclaredCopyAssignment()) {
    // Note: The following rules are largely analoguous to the copy
    // constructor rules. Note that virtual bases are not taken into account
    // for determining the argument type of the operator. Note also that
    // operators taking an object instead of a reference are allowed.
    //
    // C++ [class.copy]p10:
    //   If the class definition does not explicitly declare a copy
    //   assignment operator, one is declared implicitly.
    //   The implicitly-defined copy assignment operator for a class X
    //   will have the form
    //
    //       X& X::operator=(const X&)
    //
    //   if
    bool HasConstCopyAssignment = true;

    //       -- each direct base class B of X has a copy assignment operator
    //          whose parameter is of type const B&, const volatile B& or B,
    //          and
    for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin();
         HasConstCopyAssignment && Base != ClassDecl->bases_end(); ++Base) {
      assert(!Base->getType()->isDependentType() &&
            "Cannot generate implicit members for class with dependent bases.");
      const CXXRecordDecl *BaseClassDecl
        = cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
      const CXXMethodDecl *MD = 0;
      HasConstCopyAssignment = BaseClassDecl->hasConstCopyAssignment(Context,
                                                                     MD);
    }

    //       -- for all the nonstatic data members of X that are of a class
    //          type M (or array thereof), each such class type has a copy
    //          assignment operator whose parameter is of type const M&,
    //          const volatile M& or M.
    for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin();
         HasConstCopyAssignment && Field != ClassDecl->field_end();
         ++Field) {
      QualType FieldType = (*Field)->getType();
      if (const ArrayType *Array = Context.getAsArrayType(FieldType))
        FieldType = Array->getElementType();
      if (const RecordType *FieldClassType = FieldType->getAs<RecordType>()) {
        const CXXRecordDecl *FieldClassDecl
          = cast<CXXRecordDecl>(FieldClassType->getDecl());
        const CXXMethodDecl *MD = 0;
        HasConstCopyAssignment
          = FieldClassDecl->hasConstCopyAssignment(Context, MD);
      }
    }

    //   Otherwise, the implicitly declared copy assignment operator will
    //   have the form
    //
    //       X& X::operator=(X&)
    QualType ArgType = ClassType;
    QualType RetType = Context.getLValueReferenceType(ArgType);
    if (HasConstCopyAssignment)
      ArgType = ArgType.withConst();
    ArgType = Context.getLValueReferenceType(ArgType);

    //   An implicitly-declared copy assignment operator is an inline public
    //   member of its class.
    DeclarationName Name =
      Context.DeclarationNames.getCXXOperatorName(OO_Equal);
    CXXMethodDecl *CopyAssignment =
      CXXMethodDecl::Create(Context, ClassDecl, ClassDecl->getLocation(), Name,
                            Context.getFunctionType(RetType, &ArgType, 1,
                                                    false, 0),
                            /*TInfo=*/0, /*isStatic=*/false, /*isInline=*/true);
    CopyAssignment->setAccess(AS_public);
    CopyAssignment->setImplicit();
    CopyAssignment->setTrivial(ClassDecl->hasTrivialCopyAssignment());
    CopyAssignment->setCopyAssignment(true);

    // Add the parameter to the operator.
    ParmVarDecl *FromParam = ParmVarDecl::Create(Context, CopyAssignment,
                                                 ClassDecl->getLocation(),
                                                 /*IdentifierInfo=*/0,
                                                 ArgType, /*TInfo=*/0,
                                                 VarDecl::None, 0);
    CopyAssignment->setParams(Context, &FromParam, 1);

    // Don't call addedAssignmentOperator. There is no way to distinguish an
    // implicit from an explicit assignment operator.
    ClassDecl->addDecl(CopyAssignment);
    AddOverriddenMethods(ClassDecl, CopyAssignment);
  }

  if (!ClassDecl->hasUserDeclaredDestructor()) {
    // C++ [class.dtor]p2:
    //   If a class has no user-declared destructor, a destructor is
    //   declared implicitly. An implicitly-declared destructor is an
    //   inline public member of its class.
    DeclarationName Name
      = Context.DeclarationNames.getCXXDestructorName(ClassType);
    CXXDestructorDecl *Destructor
      = CXXDestructorDecl::Create(Context, ClassDecl,
                                  ClassDecl->getLocation(), Name,
                                  Context.getFunctionType(Context.VoidTy,
                                                          0, 0, false, 0),
                                  /*isInline=*/true,
                                  /*isImplicitlyDeclared=*/true);
    Destructor->setAccess(AS_public);
    Destructor->setImplicit();
    Destructor->setTrivial(ClassDecl->hasTrivialDestructor());
    ClassDecl->addDecl(Destructor);
    
    AddOverriddenMethods(ClassDecl, Destructor);
  }
}

void Sema::ActOnReenterTemplateScope(Scope *S, DeclPtrTy TemplateD) {
  Decl *D = TemplateD.getAs<Decl>();
  if (!D)
    return;
  
  TemplateParameterList *Params = 0;
  if (TemplateDecl *Template = dyn_cast<TemplateDecl>(D))
    Params = Template->getTemplateParameters();
  else if (ClassTemplatePartialSpecializationDecl *PartialSpec
           = dyn_cast<ClassTemplatePartialSpecializationDecl>(D))
    Params = PartialSpec->getTemplateParameters();
  else
    return;

  for (TemplateParameterList::iterator Param = Params->begin(),
                                    ParamEnd = Params->end();
       Param != ParamEnd; ++Param) {
    NamedDecl *Named = cast<NamedDecl>(*Param);
    if (Named->getDeclName()) {
      S->AddDecl(DeclPtrTy::make(Named));
      IdResolver.AddDecl(Named);
    }
  }
}

void Sema::ActOnStartDelayedMemberDeclarations(Scope *S, DeclPtrTy RecordD) {
  if (!RecordD) return;
  AdjustDeclIfTemplate(RecordD);
  CXXRecordDecl *Record = cast<CXXRecordDecl>(RecordD.getAs<Decl>());
  PushDeclContext(S, Record);
}

void Sema::ActOnFinishDelayedMemberDeclarations(Scope *S, DeclPtrTy RecordD) {
  if (!RecordD) return;
  PopDeclContext();
}

/// ActOnStartDelayedCXXMethodDeclaration - We have completed
/// parsing a top-level (non-nested) C++ class, and we are now
/// parsing those parts of the given Method declaration that could
/// not be parsed earlier (C++ [class.mem]p2), such as default
/// arguments. This action should enter the scope of the given
/// Method declaration as if we had just parsed the qualified method
/// name. However, it should not bring the parameters into scope;
/// that will be performed by ActOnDelayedCXXMethodParameter.
void Sema::ActOnStartDelayedCXXMethodDeclaration(Scope *S, DeclPtrTy MethodD) {
}

/// ActOnDelayedCXXMethodParameter - We've already started a delayed
/// C++ method declaration. We're (re-)introducing the given
/// function parameter into scope for use in parsing later parts of
/// the method declaration. For example, we could see an
/// ActOnParamDefaultArgument event for this parameter.
void Sema::ActOnDelayedCXXMethodParameter(Scope *S, DeclPtrTy ParamD) {
  if (!ParamD)
    return;

  ParmVarDecl *Param = cast<ParmVarDecl>(ParamD.getAs<Decl>());

  // If this parameter has an unparsed default argument, clear it out
  // to make way for the parsed default argument.
  if (Param->hasUnparsedDefaultArg())
    Param->setDefaultArg(0);

  S->AddDecl(DeclPtrTy::make(Param));
  if (Param->getDeclName())
    IdResolver.AddDecl(Param);
}

/// ActOnFinishDelayedCXXMethodDeclaration - We have finished
/// processing the delayed method declaration for Method. The method
/// declaration is now considered finished. There may be a separate
/// ActOnStartOfFunctionDef action later (not necessarily
/// immediately!) for this method, if it was also defined inside the
/// class body.
void Sema::ActOnFinishDelayedCXXMethodDeclaration(Scope *S, DeclPtrTy MethodD) {
  if (!MethodD)
    return;

  AdjustDeclIfTemplate(MethodD);

  FunctionDecl *Method = cast<FunctionDecl>(MethodD.getAs<Decl>());

  // Now that we have our default arguments, check the constructor
  // again. It could produce additional diagnostics or affect whether
  // the class has implicitly-declared destructors, among other
  // things.
  if (CXXConstructorDecl *Constructor = dyn_cast<CXXConstructorDecl>(Method))
    CheckConstructor(Constructor);

  // Check the default arguments, which we may have added.
  if (!Method->isInvalidDecl())
    CheckCXXDefaultArguments(Method);
}

/// CheckConstructorDeclarator - Called by ActOnDeclarator to check
/// the well-formedness of the constructor declarator @p D with type @p
/// R. If there are any errors in the declarator, this routine will
/// emit diagnostics and set the invalid bit to true.  In any case, the type
/// will be updated to reflect a well-formed type for the constructor and
/// returned.
QualType Sema::CheckConstructorDeclarator(Declarator &D, QualType R,
                                          FunctionDecl::StorageClass &SC) {
  bool isVirtual = D.getDeclSpec().isVirtualSpecified();

  // C++ [class.ctor]p3:
  //   A constructor shall not be virtual (10.3) or static (9.4). A
  //   constructor can be invoked for a const, volatile or const
  //   volatile object. A constructor shall not be declared const,
  //   volatile, or const volatile (9.3.2).
  if (isVirtual) {
    if (!D.isInvalidType())
      Diag(D.getIdentifierLoc(), diag::err_constructor_cannot_be)
        << "virtual" << SourceRange(D.getDeclSpec().getVirtualSpecLoc())
        << SourceRange(D.getIdentifierLoc());
    D.setInvalidType();
  }
  if (SC == FunctionDecl::Static) {
    if (!D.isInvalidType())
      Diag(D.getIdentifierLoc(), diag::err_constructor_cannot_be)
        << "static" << SourceRange(D.getDeclSpec().getStorageClassSpecLoc())
        << SourceRange(D.getIdentifierLoc());
    D.setInvalidType();
    SC = FunctionDecl::None;
  }

  DeclaratorChunk::FunctionTypeInfo &FTI = D.getTypeObject(0).Fun;
  if (FTI.TypeQuals != 0) {
    if (FTI.TypeQuals & Qualifiers::Const)
      Diag(D.getIdentifierLoc(), diag::err_invalid_qualified_constructor)
        << "const" << SourceRange(D.getIdentifierLoc());
    if (FTI.TypeQuals & Qualifiers::Volatile)
      Diag(D.getIdentifierLoc(), diag::err_invalid_qualified_constructor)
        << "volatile" << SourceRange(D.getIdentifierLoc());
    if (FTI.TypeQuals & Qualifiers::Restrict)
      Diag(D.getIdentifierLoc(), diag::err_invalid_qualified_constructor)
        << "restrict" << SourceRange(D.getIdentifierLoc());
  }

  // Rebuild the function type "R" without any type qualifiers (in
  // case any of the errors above fired) and with "void" as the
  // return type, since constructors don't have return types. We
  // *always* have to do this, because GetTypeForDeclarator will
  // put in a result type of "int" when none was specified.
  const FunctionProtoType *Proto = R->getAs<FunctionProtoType>();
  return Context.getFunctionType(Context.VoidTy, Proto->arg_type_begin(),
                                 Proto->getNumArgs(),
                                 Proto->isVariadic(), 0);
}

/// CheckConstructor - Checks a fully-formed constructor for
/// well-formedness, issuing any diagnostics required. Returns true if
/// the constructor declarator is invalid.
void Sema::CheckConstructor(CXXConstructorDecl *Constructor) {
  CXXRecordDecl *ClassDecl
    = dyn_cast<CXXRecordDecl>(Constructor->getDeclContext());
  if (!ClassDecl)
    return Constructor->setInvalidDecl();

  // C++ [class.copy]p3:
  //   A declaration of a constructor for a class X is ill-formed if
  //   its first parameter is of type (optionally cv-qualified) X and
  //   either there are no other parameters or else all other
  //   parameters have default arguments.
  if (!Constructor->isInvalidDecl() &&
      ((Constructor->getNumParams() == 1) ||
       (Constructor->getNumParams() > 1 &&
        Constructor->getParamDecl(1)->hasDefaultArg())) &&
      Constructor->getTemplateSpecializationKind()
                                              != TSK_ImplicitInstantiation) {
    QualType ParamType = Constructor->getParamDecl(0)->getType();
    QualType ClassTy = Context.getTagDeclType(ClassDecl);
    if (Context.getCanonicalType(ParamType).getUnqualifiedType() == ClassTy) {
      SourceLocation ParamLoc = Constructor->getParamDecl(0)->getLocation();
      Diag(ParamLoc, diag::err_constructor_byvalue_arg)
        << CodeModificationHint::CreateInsertion(ParamLoc, " const &");

      // FIXME: Rather that making the constructor invalid, we should endeavor
      // to fix the type.
      Constructor->setInvalidDecl();
    }
  }

  // Notify the class that we've added a constructor.
  ClassDecl->addedConstructor(Context, Constructor);
}

/// CheckDestructor - Checks a fully-formed destructor for well-formedness, 
/// issuing any diagnostics required. Returns true on error.
bool Sema::CheckDestructor(CXXDestructorDecl *Destructor) {
  CXXRecordDecl *RD = Destructor->getParent();
  
  if (Destructor->isVirtual()) {
    SourceLocation Loc;
    
    if (!Destructor->isImplicit())
      Loc = Destructor->getLocation();
    else
      Loc = RD->getLocation();
    
    // If we have a virtual destructor, look up the deallocation function
    FunctionDecl *OperatorDelete = 0;
    DeclarationName Name = 
    Context.DeclarationNames.getCXXOperatorName(OO_Delete);
    if (FindDeallocationFunction(Loc, RD, Name, OperatorDelete))
      return true;
    
    Destructor->setOperatorDelete(OperatorDelete);
  }
  
  return false;
}

static inline bool
FTIHasSingleVoidArgument(DeclaratorChunk::FunctionTypeInfo &FTI) {
  return (FTI.NumArgs == 1 && !FTI.isVariadic && FTI.ArgInfo[0].Ident == 0 &&
          FTI.ArgInfo[0].Param &&
          FTI.ArgInfo[0].Param.getAs<ParmVarDecl>()->getType()->isVoidType());
}

/// CheckDestructorDeclarator - Called by ActOnDeclarator to check
/// the well-formednes of the destructor declarator @p D with type @p
/// R. If there are any errors in the declarator, this routine will
/// emit diagnostics and set the declarator to invalid.  Even if this happens,
/// will be updated to reflect a well-formed type for the destructor and
/// returned.
QualType Sema::CheckDestructorDeclarator(Declarator &D,
                                         FunctionDecl::StorageClass& SC) {
  // C++ [class.dtor]p1:
  //   [...] A typedef-name that names a class is a class-name
  //   (7.1.3); however, a typedef-name that names a class shall not
  //   be used as the identifier in the declarator for a destructor
  //   declaration.
  QualType DeclaratorType = GetTypeFromParser(D.getName().DestructorName);
  if (isa<TypedefType>(DeclaratorType)) {
    Diag(D.getIdentifierLoc(), diag::err_destructor_typedef_name)
      << DeclaratorType;
    D.setInvalidType();
  }

  // C++ [class.dtor]p2:
  //   A destructor is used to destroy objects of its class type. A
  //   destructor takes no parameters, and no return type can be
  //   specified for it (not even void). The address of a destructor
  //   shall not be taken. A destructor shall not be static. A
  //   destructor can be invoked for a const, volatile or const
  //   volatile object. A destructor shall not be declared const,
  //   volatile or const volatile (9.3.2).
  if (SC == FunctionDecl::Static) {
    if (!D.isInvalidType())
      Diag(D.getIdentifierLoc(), diag::err_destructor_cannot_be)
        << "static" << SourceRange(D.getDeclSpec().getStorageClassSpecLoc())
        << SourceRange(D.getIdentifierLoc());
    SC = FunctionDecl::None;
    D.setInvalidType();
  }
  if (D.getDeclSpec().hasTypeSpecifier() && !D.isInvalidType()) {
    // Destructors don't have return types, but the parser will
    // happily parse something like:
    //
    //   class X {
    //     float ~X();
    //   };
    //
    // The return type will be eliminated later.
    Diag(D.getIdentifierLoc(), diag::err_destructor_return_type)
      << SourceRange(D.getDeclSpec().getTypeSpecTypeLoc())
      << SourceRange(D.getIdentifierLoc());
  }

  DeclaratorChunk::FunctionTypeInfo &FTI = D.getTypeObject(0).Fun;
  if (FTI.TypeQuals != 0 && !D.isInvalidType()) {
    if (FTI.TypeQuals & Qualifiers::Const)
      Diag(D.getIdentifierLoc(), diag::err_invalid_qualified_destructor)
        << "const" << SourceRange(D.getIdentifierLoc());
    if (FTI.TypeQuals & Qualifiers::Volatile)
      Diag(D.getIdentifierLoc(), diag::err_invalid_qualified_destructor)
        << "volatile" << SourceRange(D.getIdentifierLoc());
    if (FTI.TypeQuals & Qualifiers::Restrict)
      Diag(D.getIdentifierLoc(), diag::err_invalid_qualified_destructor)
        << "restrict" << SourceRange(D.getIdentifierLoc());
    D.setInvalidType();
  }

  // Make sure we don't have any parameters.
  if (FTI.NumArgs > 0 && !FTIHasSingleVoidArgument(FTI)) {
    Diag(D.getIdentifierLoc(), diag::err_destructor_with_params);

    // Delete the parameters.
    FTI.freeArgs();
    D.setInvalidType();
  }

  // Make sure the destructor isn't variadic.
  if (FTI.isVariadic) {
    Diag(D.getIdentifierLoc(), diag::err_destructor_variadic);
    D.setInvalidType();
  }

  // Rebuild the function type "R" without any type qualifiers or
  // parameters (in case any of the errors above fired) and with
  // "void" as the return type, since destructors don't have return
  // types. We *always* have to do this, because GetTypeForDeclarator
  // will put in a result type of "int" when none was specified.
  return Context.getFunctionType(Context.VoidTy, 0, 0, false, 0);
}

/// CheckConversionDeclarator - Called by ActOnDeclarator to check the
/// well-formednes of the conversion function declarator @p D with
/// type @p R. If there are any errors in the declarator, this routine
/// will emit diagnostics and return true. Otherwise, it will return
/// false. Either way, the type @p R will be updated to reflect a
/// well-formed type for the conversion operator.
void Sema::CheckConversionDeclarator(Declarator &D, QualType &R,
                                     FunctionDecl::StorageClass& SC) {
  // C++ [class.conv.fct]p1:
  //   Neither parameter types nor return type can be specified. The
  //   type of a conversion function (8.3.5) is "function taking no
  //   parameter returning conversion-type-id."
  if (SC == FunctionDecl::Static) {
    if (!D.isInvalidType())
      Diag(D.getIdentifierLoc(), diag::err_conv_function_not_member)
        << "static" << SourceRange(D.getDeclSpec().getStorageClassSpecLoc())
        << SourceRange(D.getIdentifierLoc());
    D.setInvalidType();
    SC = FunctionDecl::None;
  }
  if (D.getDeclSpec().hasTypeSpecifier() && !D.isInvalidType()) {
    // Conversion functions don't have return types, but the parser will
    // happily parse something like:
    //
    //   class X {
    //     float operator bool();
    //   };
    //
    // The return type will be changed later anyway.
    Diag(D.getIdentifierLoc(), diag::err_conv_function_return_type)
      << SourceRange(D.getDeclSpec().getTypeSpecTypeLoc())
      << SourceRange(D.getIdentifierLoc());
  }

  // Make sure we don't have any parameters.
  if (R->getAs<FunctionProtoType>()->getNumArgs() > 0) {
    Diag(D.getIdentifierLoc(), diag::err_conv_function_with_params);

    // Delete the parameters.
    D.getTypeObject(0).Fun.freeArgs();
    D.setInvalidType();
  }

  // Make sure the conversion function isn't variadic.
  if (R->getAs<FunctionProtoType>()->isVariadic() && !D.isInvalidType()) {
    Diag(D.getIdentifierLoc(), diag::err_conv_function_variadic);
    D.setInvalidType();
  }

  // C++ [class.conv.fct]p4:
  //   The conversion-type-id shall not represent a function type nor
  //   an array type.
  QualType ConvType = GetTypeFromParser(D.getName().ConversionFunctionId);
  if (ConvType->isArrayType()) {
    Diag(D.getIdentifierLoc(), diag::err_conv_function_to_array);
    ConvType = Context.getPointerType(ConvType);
    D.setInvalidType();
  } else if (ConvType->isFunctionType()) {
    Diag(D.getIdentifierLoc(), diag::err_conv_function_to_function);
    ConvType = Context.getPointerType(ConvType);
    D.setInvalidType();
  }

  // Rebuild the function type "R" without any parameters (in case any
  // of the errors above fired) and with the conversion type as the
  // return type.
  R = Context.getFunctionType(ConvType, 0, 0, false,
                              R->getAs<FunctionProtoType>()->getTypeQuals());

  // C++0x explicit conversion operators.
  if (D.getDeclSpec().isExplicitSpecified() && !getLangOptions().CPlusPlus0x)
    Diag(D.getDeclSpec().getExplicitSpecLoc(),
         diag::warn_explicit_conversion_functions)
      << SourceRange(D.getDeclSpec().getExplicitSpecLoc());
}

/// ActOnConversionDeclarator - Called by ActOnDeclarator to complete
/// the declaration of the given C++ conversion function. This routine
/// is responsible for recording the conversion function in the C++
/// class, if possible.
Sema::DeclPtrTy Sema::ActOnConversionDeclarator(CXXConversionDecl *Conversion) {
  assert(Conversion && "Expected to receive a conversion function declaration");

  CXXRecordDecl *ClassDecl = cast<CXXRecordDecl>(Conversion->getDeclContext());

  // Make sure we aren't redeclaring the conversion function.
  QualType ConvType = Context.getCanonicalType(Conversion->getConversionType());

  // C++ [class.conv.fct]p1:
  //   [...] A conversion function is never used to convert a
  //   (possibly cv-qualified) object to the (possibly cv-qualified)
  //   same object type (or a reference to it), to a (possibly
  //   cv-qualified) base class of that type (or a reference to it),
  //   or to (possibly cv-qualified) void.
  // FIXME: Suppress this warning if the conversion function ends up being a
  // virtual function that overrides a virtual function in a base class.
  QualType ClassType
    = Context.getCanonicalType(Context.getTypeDeclType(ClassDecl));
  if (const ReferenceType *ConvTypeRef = ConvType->getAs<ReferenceType>())
    ConvType = ConvTypeRef->getPointeeType();
  if (ConvType->isRecordType()) {
    ConvType = Context.getCanonicalType(ConvType).getUnqualifiedType();
    if (ConvType == ClassType)
      Diag(Conversion->getLocation(), diag::warn_conv_to_self_not_used)
        << ClassType;
    else if (IsDerivedFrom(ClassType, ConvType))
      Diag(Conversion->getLocation(), diag::warn_conv_to_base_not_used)
        <<  ClassType << ConvType;
  } else if (ConvType->isVoidType()) {
    Diag(Conversion->getLocation(), diag::warn_conv_to_void_not_used)
      << ClassType << ConvType;
  }

  if (Conversion->getPreviousDeclaration()) {
    const NamedDecl *ExpectedPrevDecl = Conversion->getPreviousDeclaration();
    if (FunctionTemplateDecl *ConversionTemplate
          = Conversion->getDescribedFunctionTemplate())
      ExpectedPrevDecl = ConversionTemplate->getPreviousDeclaration();
    if (ClassDecl->replaceConversion(ExpectedPrevDecl, Conversion))
      return DeclPtrTy::make(Conversion);
    assert(Conversion->isInvalidDecl() && "Conversion should not get here.");
  } else if (FunctionTemplateDecl *ConversionTemplate
               = Conversion->getDescribedFunctionTemplate())
    ClassDecl->addConversionFunction(ConversionTemplate);
  else if (!Conversion->getPrimaryTemplate()) // ignore specializations
    ClassDecl->addConversionFunction(Conversion);

  return DeclPtrTy::make(Conversion);
}

//===----------------------------------------------------------------------===//
// Namespace Handling
//===----------------------------------------------------------------------===//

/// ActOnStartNamespaceDef - This is called at the start of a namespace
/// definition.
Sema::DeclPtrTy Sema::ActOnStartNamespaceDef(Scope *NamespcScope,
                                             SourceLocation IdentLoc,
                                             IdentifierInfo *II,
                                             SourceLocation LBrace) {
  NamespaceDecl *Namespc =
      NamespaceDecl::Create(Context, CurContext, IdentLoc, II);
  Namespc->setLBracLoc(LBrace);

  Scope *DeclRegionScope = NamespcScope->getParent();

  if (II) {
    // C++ [namespace.def]p2:
    // The identifier in an original-namespace-definition shall not have been
    // previously defined in the declarative region in which the
    // original-namespace-definition appears. The identifier in an
    // original-namespace-definition is the name of the namespace. Subsequently
    // in that declarative region, it is treated as an original-namespace-name.

    NamedDecl *PrevDecl
      = LookupSingleName(DeclRegionScope, II, LookupOrdinaryName,
                         ForRedeclaration);

    if (NamespaceDecl *OrigNS = dyn_cast_or_null<NamespaceDecl>(PrevDecl)) {
      // This is an extended namespace definition.
      // Attach this namespace decl to the chain of extended namespace
      // definitions.
      OrigNS->setNextNamespace(Namespc);
      Namespc->setOriginalNamespace(OrigNS->getOriginalNamespace());

      // Remove the previous declaration from the scope.
      if (DeclRegionScope->isDeclScope(DeclPtrTy::make(OrigNS))) {
        IdResolver.RemoveDecl(OrigNS);
        DeclRegionScope->RemoveDecl(DeclPtrTy::make(OrigNS));
      }
    } else if (PrevDecl) {
      // This is an invalid name redefinition.
      Diag(Namespc->getLocation(), diag::err_redefinition_different_kind)
       << Namespc->getDeclName();
      Diag(PrevDecl->getLocation(), diag::note_previous_definition);
      Namespc->setInvalidDecl();
      // Continue on to push Namespc as current DeclContext and return it.
    } else if (II->isStr("std") && 
               CurContext->getLookupContext()->isTranslationUnit()) {
      // This is the first "real" definition of the namespace "std", so update
      // our cache of the "std" namespace to point at this definition.
      if (StdNamespace) {
        // We had already defined a dummy namespace "std". Link this new 
        // namespace definition to the dummy namespace "std".
        StdNamespace->setNextNamespace(Namespc);
        StdNamespace->setLocation(IdentLoc);
        Namespc->setOriginalNamespace(StdNamespace->getOriginalNamespace());
      }
      
      // Make our StdNamespace cache point at the first real definition of the
      // "std" namespace.
      StdNamespace = Namespc;
    }

    PushOnScopeChains(Namespc, DeclRegionScope);
  } else {
    // Anonymous namespaces.
    assert(Namespc->isAnonymousNamespace());
    CurContext->addDecl(Namespc);

    // Link the anonymous namespace into its parent.
    NamespaceDecl *PrevDecl;
    DeclContext *Parent = CurContext->getLookupContext();
    if (TranslationUnitDecl *TU = dyn_cast<TranslationUnitDecl>(Parent)) {
      PrevDecl = TU->getAnonymousNamespace();
      TU->setAnonymousNamespace(Namespc);
    } else {
      NamespaceDecl *ND = cast<NamespaceDecl>(Parent);
      PrevDecl = ND->getAnonymousNamespace();
      ND->setAnonymousNamespace(Namespc);
    }

    // Link the anonymous namespace with its previous declaration.
    if (PrevDecl) {
      assert(PrevDecl->isAnonymousNamespace());
      assert(!PrevDecl->getNextNamespace());
      Namespc->setOriginalNamespace(PrevDecl->getOriginalNamespace());
      PrevDecl->setNextNamespace(Namespc);
    }

    // C++ [namespace.unnamed]p1.  An unnamed-namespace-definition
    //   behaves as if it were replaced by
    //     namespace unique { /* empty body */ }
    //     using namespace unique;
    //     namespace unique { namespace-body }
    //   where all occurrences of 'unique' in a translation unit are
    //   replaced by the same identifier and this identifier differs
    //   from all other identifiers in the entire program.

    // We just create the namespace with an empty name and then add an
    // implicit using declaration, just like the standard suggests.
    //
    // CodeGen enforces the "universally unique" aspect by giving all
    // declarations semantically contained within an anonymous
    // namespace internal linkage.

    if (!PrevDecl) {
      UsingDirectiveDecl* UD
        = UsingDirectiveDecl::Create(Context, CurContext,
                                     /* 'using' */ LBrace,
                                     /* 'namespace' */ SourceLocation(),
                                     /* qualifier */ SourceRange(),
                                     /* NNS */ NULL,
                                     /* identifier */ SourceLocation(),
                                     Namespc,
                                     /* Ancestor */ CurContext);
      UD->setImplicit();
      CurContext->addDecl(UD);
    }
  }

  // Although we could have an invalid decl (i.e. the namespace name is a
  // redefinition), push it as current DeclContext and try to continue parsing.
  // FIXME: We should be able to push Namespc here, so that the each DeclContext
  // for the namespace has the declarations that showed up in that particular
  // namespace definition.
  PushDeclContext(NamespcScope, Namespc);
  return DeclPtrTy::make(Namespc);
}

/// getNamespaceDecl - Returns the namespace a decl represents. If the decl
/// is a namespace alias, returns the namespace it points to.
static inline NamespaceDecl *getNamespaceDecl(NamedDecl *D) {
  if (NamespaceAliasDecl *AD = dyn_cast_or_null<NamespaceAliasDecl>(D))
    return AD->getNamespace();
  return dyn_cast_or_null<NamespaceDecl>(D);
}

/// ActOnFinishNamespaceDef - This callback is called after a namespace is
/// exited. Decl is the DeclTy returned by ActOnStartNamespaceDef.
void Sema::ActOnFinishNamespaceDef(DeclPtrTy D, SourceLocation RBrace) {
  Decl *Dcl = D.getAs<Decl>();
  NamespaceDecl *Namespc = dyn_cast_or_null<NamespaceDecl>(Dcl);
  assert(Namespc && "Invalid parameter, expected NamespaceDecl");
  Namespc->setRBracLoc(RBrace);
  PopDeclContext();
}

Sema::DeclPtrTy Sema::ActOnUsingDirective(Scope *S,
                                          SourceLocation UsingLoc,
                                          SourceLocation NamespcLoc,
                                          const CXXScopeSpec &SS,
                                          SourceLocation IdentLoc,
                                          IdentifierInfo *NamespcName,
                                          AttributeList *AttrList) {
  assert(!SS.isInvalid() && "Invalid CXXScopeSpec.");
  assert(NamespcName && "Invalid NamespcName.");
  assert(IdentLoc.isValid() && "Invalid NamespceName location.");
  assert(S->getFlags() & Scope::DeclScope && "Invalid Scope.");

  UsingDirectiveDecl *UDir = 0;

  // Lookup namespace name.
  LookupResult R(*this, NamespcName, IdentLoc, LookupNamespaceName);
  LookupParsedName(R, S, &SS);
  if (R.isAmbiguous())
    return DeclPtrTy();

  if (!R.empty()) {
    NamedDecl *Named = R.getFoundDecl();
    assert((isa<NamespaceDecl>(Named) || isa<NamespaceAliasDecl>(Named))
        && "expected namespace decl");
    // C++ [namespace.udir]p1:
    //   A using-directive specifies that the names in the nominated
    //   namespace can be used in the scope in which the
    //   using-directive appears after the using-directive. During
    //   unqualified name lookup (3.4.1), the names appear as if they
    //   were declared in the nearest enclosing namespace which
    //   contains both the using-directive and the nominated
    //   namespace. [Note: in this context, "contains" means "contains
    //   directly or indirectly". ]

    // Find enclosing context containing both using-directive and
    // nominated namespace.
    NamespaceDecl *NS = getNamespaceDecl(Named);
    DeclContext *CommonAncestor = cast<DeclContext>(NS);
    while (CommonAncestor && !CommonAncestor->Encloses(CurContext))
      CommonAncestor = CommonAncestor->getParent();

    UDir = UsingDirectiveDecl::Create(Context, CurContext, UsingLoc, NamespcLoc,
                                      SS.getRange(),
                                      (NestedNameSpecifier *)SS.getScopeRep(),
                                      IdentLoc, Named, CommonAncestor);
    PushUsingDirective(S, UDir);
  } else {
    Diag(IdentLoc, diag::err_expected_namespace_name) << SS.getRange();
  }

  // FIXME: We ignore attributes for now.
  delete AttrList;
  return DeclPtrTy::make(UDir);
}

void Sema::PushUsingDirective(Scope *S, UsingDirectiveDecl *UDir) {
  // If scope has associated entity, then using directive is at namespace
  // or translation unit scope. We add UsingDirectiveDecls, into
  // it's lookup structure.
  if (DeclContext *Ctx = static_cast<DeclContext*>(S->getEntity()))
    Ctx->addDecl(UDir);
  else
    // Otherwise it is block-sope. using-directives will affect lookup
    // only to the end of scope.
    S->PushUsingDirective(DeclPtrTy::make(UDir));
}


Sema::DeclPtrTy Sema::ActOnUsingDeclaration(Scope *S,
                                            AccessSpecifier AS,
                                            bool HasUsingKeyword,
                                            SourceLocation UsingLoc,
                                            const CXXScopeSpec &SS,
                                            UnqualifiedId &Name,
                                            AttributeList *AttrList,
                                            bool IsTypeName,
                                            SourceLocation TypenameLoc) {
  assert(S->getFlags() & Scope::DeclScope && "Invalid Scope.");

  switch (Name.getKind()) {
  case UnqualifiedId::IK_Identifier:
  case UnqualifiedId::IK_OperatorFunctionId:
  case UnqualifiedId::IK_LiteralOperatorId:
  case UnqualifiedId::IK_ConversionFunctionId:
    break;
      
  case UnqualifiedId::IK_ConstructorName:
    // C++0x inherited constructors.
    if (getLangOptions().CPlusPlus0x) break;

    Diag(Name.getSourceRange().getBegin(), diag::err_using_decl_constructor)
      << SS.getRange();
    return DeclPtrTy();
      
  case UnqualifiedId::IK_DestructorName:
    Diag(Name.getSourceRange().getBegin(), diag::err_using_decl_destructor)
      << SS.getRange();
    return DeclPtrTy();
      
  case UnqualifiedId::IK_TemplateId:
    Diag(Name.getSourceRange().getBegin(), diag::err_using_decl_template_id)
      << SourceRange(Name.TemplateId->LAngleLoc, Name.TemplateId->RAngleLoc);
    return DeclPtrTy();
  }
  
  DeclarationName TargetName = GetNameFromUnqualifiedId(Name);
  if (!TargetName)
    return DeclPtrTy();

  // Warn about using declarations.
  // TODO: store that the declaration was written without 'using' and
  // talk about access decls instead of using decls in the
  // diagnostics.
  if (!HasUsingKeyword) {
    UsingLoc = Name.getSourceRange().getBegin();
    
    Diag(UsingLoc, diag::warn_access_decl_deprecated)
      << CodeModificationHint::CreateInsertion(SS.getRange().getBegin(),
                                               "using ");
  }

  NamedDecl *UD = BuildUsingDeclaration(S, AS, UsingLoc, SS,
                                        Name.getSourceRange().getBegin(),
                                        TargetName, AttrList,
                                        /* IsInstantiation */ false,
                                        IsTypeName, TypenameLoc);
  if (UD)
    PushOnScopeChains(UD, S, /*AddToContext*/ false);

  return DeclPtrTy::make(UD);
}

/// Determines whether to create a using shadow decl for a particular
/// decl, given the set of decls existing prior to this using lookup.
bool Sema::CheckUsingShadowDecl(UsingDecl *Using, NamedDecl *Orig,
                                const LookupResult &Previous) {
  // Diagnose finding a decl which is not from a base class of the
  // current class.  We do this now because there are cases where this
  // function will silently decide not to build a shadow decl, which
  // will pre-empt further diagnostics.
  //
  // We don't need to do this in C++0x because we do the check once on
  // the qualifier.
  //
  // FIXME: diagnose the following if we care enough:
  //   struct A { int foo; };
  //   struct B : A { using A::foo; };
  //   template <class T> struct C : A {};
  //   template <class T> struct D : C<T> { using B::foo; } // <---
  // This is invalid (during instantiation) in C++03 because B::foo
  // resolves to the using decl in B, which is not a base class of D<T>.
  // We can't diagnose it immediately because C<T> is an unknown
  // specialization.  The UsingShadowDecl in D<T> then points directly
  // to A::foo, which will look well-formed when we instantiate.
  // The right solution is to not collapse the shadow-decl chain.
  if (!getLangOptions().CPlusPlus0x && CurContext->isRecord()) {
    DeclContext *OrigDC = Orig->getDeclContext();

    // Handle enums and anonymous structs.
    if (isa<EnumDecl>(OrigDC)) OrigDC = OrigDC->getParent();
    CXXRecordDecl *OrigRec = cast<CXXRecordDecl>(OrigDC);
    while (OrigRec->isAnonymousStructOrUnion())
      OrigRec = cast<CXXRecordDecl>(OrigRec->getDeclContext());

    if (cast<CXXRecordDecl>(CurContext)->isProvablyNotDerivedFrom(OrigRec)) {
      if (OrigDC == CurContext) {
        Diag(Using->getLocation(),
             diag::err_using_decl_nested_name_specifier_is_current_class)
          << Using->getNestedNameRange();
        Diag(Orig->getLocation(), diag::note_using_decl_target);
        return true;
      }

      Diag(Using->getNestedNameRange().getBegin(),
           diag::err_using_decl_nested_name_specifier_is_not_base_class)
        << Using->getTargetNestedNameDecl()
        << cast<CXXRecordDecl>(CurContext)
        << Using->getNestedNameRange();
      Diag(Orig->getLocation(), diag::note_using_decl_target);
      return true;
    }
  }

  if (Previous.empty()) return false;

  NamedDecl *Target = Orig;
  if (isa<UsingShadowDecl>(Target))
    Target = cast<UsingShadowDecl>(Target)->getTargetDecl();

  // If the target happens to be one of the previous declarations, we
  // don't have a conflict.
  // 
  // FIXME: but we might be increasing its access, in which case we
  // should redeclare it.
  NamedDecl *NonTag = 0, *Tag = 0;
  for (LookupResult::iterator I = Previous.begin(), E = Previous.end();
         I != E; ++I) {
    NamedDecl *D = (*I)->getUnderlyingDecl();
    if (D->getCanonicalDecl() == Target->getCanonicalDecl())
      return false;

    (isa<TagDecl>(D) ? Tag : NonTag) = D;
  }

  if (Target->isFunctionOrFunctionTemplate()) {
    FunctionDecl *FD;
    if (isa<FunctionTemplateDecl>(Target))
      FD = cast<FunctionTemplateDecl>(Target)->getTemplatedDecl();
    else
      FD = cast<FunctionDecl>(Target);

    NamedDecl *OldDecl = 0;
    switch (CheckOverload(FD, Previous, OldDecl)) {
    case Ovl_Overload:
      return false;

    case Ovl_NonFunction:
      Diag(Using->getLocation(), diag::err_using_decl_conflict);
      break;
      
    // We found a decl with the exact signature.
    case Ovl_Match:
      if (isa<UsingShadowDecl>(OldDecl)) {
        // Silently ignore the possible conflict.
        return false;
      }

      // If we're in a record, we want to hide the target, so we
      // return true (without a diagnostic) to tell the caller not to
      // build a shadow decl.
      if (CurContext->isRecord())
        return true;

      // If we're not in a record, this is an error.
      Diag(Using->getLocation(), diag::err_using_decl_conflict);
      break;
    }

    Diag(Target->getLocation(), diag::note_using_decl_target);
    Diag(OldDecl->getLocation(), diag::note_using_decl_conflict);
    return true;
  }

  // Target is not a function.

  if (isa<TagDecl>(Target)) {
    // No conflict between a tag and a non-tag.
    if (!Tag) return false;

    Diag(Using->getLocation(), diag::err_using_decl_conflict);
    Diag(Target->getLocation(), diag::note_using_decl_target);
    Diag(Tag->getLocation(), diag::note_using_decl_conflict);
    return true;
  }

  // No conflict between a tag and a non-tag.
  if (!NonTag) return false;

  Diag(Using->getLocation(), diag::err_using_decl_conflict);
  Diag(Target->getLocation(), diag::note_using_decl_target);
  Diag(NonTag->getLocation(), diag::note_using_decl_conflict);
  return true;
}

/// Builds a shadow declaration corresponding to a 'using' declaration.
UsingShadowDecl *Sema::BuildUsingShadowDecl(Scope *S,
                                            UsingDecl *UD,
                                            NamedDecl *Orig) {

  // If we resolved to another shadow declaration, just coalesce them.
  NamedDecl *Target = Orig;
  if (isa<UsingShadowDecl>(Target)) {
    Target = cast<UsingShadowDecl>(Target)->getTargetDecl();
    assert(!isa<UsingShadowDecl>(Target) && "nested shadow declaration");
  }
  
  UsingShadowDecl *Shadow
    = UsingShadowDecl::Create(Context, CurContext,
                              UD->getLocation(), UD, Target);
  UD->addShadowDecl(Shadow);

  if (S)
    PushOnScopeChains(Shadow, S);
  else
    CurContext->addDecl(Shadow);
  Shadow->setAccess(UD->getAccess());

  if (Orig->isInvalidDecl() || UD->isInvalidDecl())
    Shadow->setInvalidDecl();

  return Shadow;
}

/// Hides a using shadow declaration.  This is required by the current
/// using-decl implementation when a resolvable using declaration in a
/// class is followed by a declaration which would hide or override
/// one or more of the using decl's targets; for example:
///
///   struct Base { void foo(int); };
///   struct Derived : Base {
///     using Base::foo;
///     void foo(int);
///   };
///
/// The governing language is C++03 [namespace.udecl]p12:
///
///   When a using-declaration brings names from a base class into a
///   derived class scope, member functions in the derived class
///   override and/or hide member functions with the same name and
///   parameter types in a base class (rather than conflicting).
///
/// There are two ways to implement this:
///   (1) optimistically create shadow decls when they're not hidden
///       by existing declarations, or
///   (2) don't create any shadow decls (or at least don't make them
///       visible) until we've fully parsed/instantiated the class.
/// The problem with (1) is that we might have to retroactively remove
/// a shadow decl, which requires several O(n) operations because the
/// decl structures are (very reasonably) not designed for removal.
/// (2) avoids this but is very fiddly and phase-dependent.
void Sema::HideUsingShadowDecl(Scope *S, UsingShadowDecl *Shadow) {
  // Remove it from the DeclContext...
  Shadow->getDeclContext()->removeDecl(Shadow);

  // ...and the scope, if applicable...
  if (S) {
    S->RemoveDecl(DeclPtrTy::make(static_cast<Decl*>(Shadow)));
    IdResolver.RemoveDecl(Shadow);
  }

  // ...and the using decl.
  Shadow->getUsingDecl()->removeShadowDecl(Shadow);

  // TODO: complain somehow if Shadow was used.  It shouldn't
  // be possible for this to happen, because 
}

/// Builds a using declaration.
///
/// \param IsInstantiation - Whether this call arises from an
///   instantiation of an unresolved using declaration.  We treat
///   the lookup differently for these declarations.
NamedDecl *Sema::BuildUsingDeclaration(Scope *S, AccessSpecifier AS,
                                       SourceLocation UsingLoc,
                                       const CXXScopeSpec &SS,
                                       SourceLocation IdentLoc,
                                       DeclarationName Name,
                                       AttributeList *AttrList,
                                       bool IsInstantiation,
                                       bool IsTypeName,
                                       SourceLocation TypenameLoc) {
  assert(!SS.isInvalid() && "Invalid CXXScopeSpec.");
  assert(IdentLoc.isValid() && "Invalid TargetName location.");

  // FIXME: We ignore attributes for now.
  delete AttrList;

  if (SS.isEmpty()) {
    Diag(IdentLoc, diag::err_using_requires_qualname);
    return 0;
  }

  // Do the redeclaration lookup in the current scope.
  LookupResult Previous(*this, Name, IdentLoc, LookupUsingDeclName,
                        ForRedeclaration);
  Previous.setHideTags(false);
  if (S) {
    LookupName(Previous, S);

    // It is really dumb that we have to do this.
    LookupResult::Filter F = Previous.makeFilter();
    while (F.hasNext()) {
      NamedDecl *D = F.next();
      if (!isDeclInScope(D, CurContext, S))
        F.erase();
    }
    F.done();
  } else {
    assert(IsInstantiation && "no scope in non-instantiation");
    assert(CurContext->isRecord() && "scope not record in instantiation");
    LookupQualifiedName(Previous, CurContext);
  }

  NestedNameSpecifier *NNS =
    static_cast<NestedNameSpecifier *>(SS.getScopeRep());

  // Check for invalid redeclarations.
  if (CheckUsingDeclRedeclaration(UsingLoc, IsTypeName, SS, IdentLoc, Previous))
    return 0;

  // Check for bad qualifiers.
  if (CheckUsingDeclQualifier(UsingLoc, SS, IdentLoc))
    return 0;

  DeclContext *LookupContext = computeDeclContext(SS);
  NamedDecl *D;
  if (!LookupContext) {
    if (IsTypeName) {
      // FIXME: not all declaration name kinds are legal here
      D = UnresolvedUsingTypenameDecl::Create(Context, CurContext,
                                              UsingLoc, TypenameLoc,
                                              SS.getRange(), NNS,
                                              IdentLoc, Name);
    } else {
      D = UnresolvedUsingValueDecl::Create(Context, CurContext,
                                           UsingLoc, SS.getRange(), NNS,
                                           IdentLoc, Name);
    }
  } else {
    D = UsingDecl::Create(Context, CurContext, IdentLoc,
                          SS.getRange(), UsingLoc, NNS, Name,
                          IsTypeName);
  }
  D->setAccess(AS);
  CurContext->addDecl(D);

  if (!LookupContext) return D;
  UsingDecl *UD = cast<UsingDecl>(D);

  if (RequireCompleteDeclContext(SS)) {
    UD->setInvalidDecl();
    return UD;
  }

  // Look up the target name.

  LookupResult R(*this, Name, IdentLoc, LookupOrdinaryName);

  // Unlike most lookups, we don't always want to hide tag
  // declarations: tag names are visible through the using declaration
  // even if hidden by ordinary names, *except* in a dependent context
  // where it's important for the sanity of two-phase lookup.
  if (!IsInstantiation)
    R.setHideTags(false);

  LookupQualifiedName(R, LookupContext);

  if (R.empty()) {
    Diag(IdentLoc, diag::err_no_member) 
      << Name << LookupContext << SS.getRange();
    UD->setInvalidDecl();
    return UD;
  }

  if (R.isAmbiguous()) {
    UD->setInvalidDecl();
    return UD;
  }

  if (IsTypeName) {
    // If we asked for a typename and got a non-type decl, error out.
    if (!R.getAsSingle<TypeDecl>()) {
      Diag(IdentLoc, diag::err_using_typename_non_type);
      for (LookupResult::iterator I = R.begin(), E = R.end(); I != E; ++I)
        Diag((*I)->getUnderlyingDecl()->getLocation(),
             diag::note_using_decl_target);
      UD->setInvalidDecl();
      return UD;
    }
  } else {
    // If we asked for a non-typename and we got a type, error out,
    // but only if this is an instantiation of an unresolved using
    // decl.  Otherwise just silently find the type name.
    if (IsInstantiation && R.getAsSingle<TypeDecl>()) {
      Diag(IdentLoc, diag::err_using_dependent_value_is_type);
      Diag(R.getFoundDecl()->getLocation(), diag::note_using_decl_target);
      UD->setInvalidDecl();
      return UD;
    }
  }

  // C++0x N2914 [namespace.udecl]p6:
  // A using-declaration shall not name a namespace.
  if (R.getAsSingle<NamespaceDecl>()) {
    Diag(IdentLoc, diag::err_using_decl_can_not_refer_to_namespace)
      << SS.getRange();
    UD->setInvalidDecl();
    return UD;
  }

  for (LookupResult::iterator I = R.begin(), E = R.end(); I != E; ++I) {
    if (!CheckUsingShadowDecl(UD, *I, Previous))
      BuildUsingShadowDecl(S, UD, *I);
  }

  return UD;
}

/// Checks that the given using declaration is not an invalid
/// redeclaration.  Note that this is checking only for the using decl
/// itself, not for any ill-formedness among the UsingShadowDecls.
bool Sema::CheckUsingDeclRedeclaration(SourceLocation UsingLoc,
                                       bool isTypeName,
                                       const CXXScopeSpec &SS,
                                       SourceLocation NameLoc,
                                       const LookupResult &Prev) {
  // C++03 [namespace.udecl]p8:
  // C++0x [namespace.udecl]p10:
  //   A using-declaration is a declaration and can therefore be used
  //   repeatedly where (and only where) multiple declarations are
  //   allowed.
  // That's only in file contexts.
  if (CurContext->getLookupContext()->isFileContext())
    return false;

  NestedNameSpecifier *Qual
    = static_cast<NestedNameSpecifier*>(SS.getScopeRep());

  for (LookupResult::iterator I = Prev.begin(), E = Prev.end(); I != E; ++I) {
    NamedDecl *D = *I;

    bool DTypename;
    NestedNameSpecifier *DQual;
    if (UsingDecl *UD = dyn_cast<UsingDecl>(D)) {
      DTypename = UD->isTypeName();
      DQual = UD->getTargetNestedNameDecl();
    } else if (UnresolvedUsingValueDecl *UD
                 = dyn_cast<UnresolvedUsingValueDecl>(D)) {
      DTypename = false;
      DQual = UD->getTargetNestedNameSpecifier();
    } else if (UnresolvedUsingTypenameDecl *UD
                 = dyn_cast<UnresolvedUsingTypenameDecl>(D)) {
      DTypename = true;
      DQual = UD->getTargetNestedNameSpecifier();
    } else continue;

    // using decls differ if one says 'typename' and the other doesn't.
    // FIXME: non-dependent using decls?
    if (isTypeName != DTypename) continue;

    // using decls differ if they name different scopes (but note that
    // template instantiation can cause this check to trigger when it
    // didn't before instantiation).
    if (Context.getCanonicalNestedNameSpecifier(Qual) !=
        Context.getCanonicalNestedNameSpecifier(DQual))
      continue;

    Diag(NameLoc, diag::err_using_decl_redeclaration) << SS.getRange();
    Diag(D->getLocation(), diag::note_using_decl) << 1;
    return true;
  }

  return false;
}


/// Checks that the given nested-name qualifier used in a using decl
/// in the current context is appropriately related to the current
/// scope.  If an error is found, diagnoses it and returns true.
bool Sema::CheckUsingDeclQualifier(SourceLocation UsingLoc,
                                   const CXXScopeSpec &SS,
                                   SourceLocation NameLoc) {
  DeclContext *NamedContext = computeDeclContext(SS);

  if (!CurContext->isRecord()) {
    // C++03 [namespace.udecl]p3:
    // C++0x [namespace.udecl]p8:
    //   A using-declaration for a class member shall be a member-declaration.

    // If we weren't able to compute a valid scope, it must be a
    // dependent class scope.
    if (!NamedContext || NamedContext->isRecord()) {
      Diag(NameLoc, diag::err_using_decl_can_not_refer_to_class_member)
        << SS.getRange();
      return true;
    }

    // Otherwise, everything is known to be fine.
    return false;
  }

  // The current scope is a record.

  // If the named context is dependent, we can't decide much.
  if (!NamedContext) {
    // FIXME: in C++0x, we can diagnose if we can prove that the
    // nested-name-specifier does not refer to a base class, which is
    // still possible in some cases.

    // Otherwise we have to conservatively report that things might be
    // okay.
    return false;
  }

  if (!NamedContext->isRecord()) {
    // Ideally this would point at the last name in the specifier,
    // but we don't have that level of source info.
    Diag(SS.getRange().getBegin(),
         diag::err_using_decl_nested_name_specifier_is_not_class)
      << (NestedNameSpecifier*) SS.getScopeRep() << SS.getRange();
    return true;
  }

  if (getLangOptions().CPlusPlus0x) {
    // C++0x [namespace.udecl]p3:
    //   In a using-declaration used as a member-declaration, the
    //   nested-name-specifier shall name a base class of the class
    //   being defined.

    if (cast<CXXRecordDecl>(CurContext)->isProvablyNotDerivedFrom(
                                 cast<CXXRecordDecl>(NamedContext))) {
      if (CurContext == NamedContext) {
        Diag(NameLoc,
             diag::err_using_decl_nested_name_specifier_is_current_class)
          << SS.getRange();
        return true;
      }

      Diag(SS.getRange().getBegin(),
           diag::err_using_decl_nested_name_specifier_is_not_base_class)
        << (NestedNameSpecifier*) SS.getScopeRep()
        << cast<CXXRecordDecl>(CurContext)
        << SS.getRange();
      return true;
    }

    return false;
  }

  // C++03 [namespace.udecl]p4:
  //   A using-declaration used as a member-declaration shall refer
  //   to a member of a base class of the class being defined [etc.].

  // Salient point: SS doesn't have to name a base class as long as
  // lookup only finds members from base classes.  Therefore we can
  // diagnose here only if we can prove that that can't happen,
  // i.e. if the class hierarchies provably don't intersect.

  // TODO: it would be nice if "definitely valid" results were cached
  // in the UsingDecl and UsingShadowDecl so that these checks didn't
  // need to be repeated.

  struct UserData {
    llvm::DenseSet<const CXXRecordDecl*> Bases;

    static bool collect(const CXXRecordDecl *Base, void *OpaqueData) {
      UserData *Data = reinterpret_cast<UserData*>(OpaqueData);
      Data->Bases.insert(Base);
      return true;
    }

    bool hasDependentBases(const CXXRecordDecl *Class) {
      return !Class->forallBases(collect, this);
    }

    /// Returns true if the base is dependent or is one of the
    /// accumulated base classes.
    static bool doesNotContain(const CXXRecordDecl *Base, void *OpaqueData) {
      UserData *Data = reinterpret_cast<UserData*>(OpaqueData);
      return !Data->Bases.count(Base);
    }

    bool mightShareBases(const CXXRecordDecl *Class) {
      return Bases.count(Class) || !Class->forallBases(doesNotContain, this);
    }
  };

  UserData Data;

  // Returns false if we find a dependent base.
  if (Data.hasDependentBases(cast<CXXRecordDecl>(CurContext)))
    return false;

  // Returns false if the class has a dependent base or if it or one
  // of its bases is present in the base set of the current context.
  if (Data.mightShareBases(cast<CXXRecordDecl>(NamedContext)))
    return false;

  Diag(SS.getRange().getBegin(),
       diag::err_using_decl_nested_name_specifier_is_not_base_class)
    << (NestedNameSpecifier*) SS.getScopeRep()
    << cast<CXXRecordDecl>(CurContext)
    << SS.getRange();

  return true;
}

Sema::DeclPtrTy Sema::ActOnNamespaceAliasDef(Scope *S,
                                             SourceLocation NamespaceLoc,
                                             SourceLocation AliasLoc,
                                             IdentifierInfo *Alias,
                                             const CXXScopeSpec &SS,
                                             SourceLocation IdentLoc,
                                             IdentifierInfo *Ident) {

  // Lookup the namespace name.
  LookupResult R(*this, Ident, IdentLoc, LookupNamespaceName);
  LookupParsedName(R, S, &SS);

  // Check if we have a previous declaration with the same name.
  if (NamedDecl *PrevDecl
        = LookupSingleName(S, Alias, LookupOrdinaryName, ForRedeclaration)) {
    if (NamespaceAliasDecl *AD = dyn_cast<NamespaceAliasDecl>(PrevDecl)) {
      // We already have an alias with the same name that points to the same
      // namespace, so don't create a new one.
      if (!R.isAmbiguous() && !R.empty() &&
          AD->getNamespace() == getNamespaceDecl(R.getFoundDecl()))
        return DeclPtrTy();
    }

    unsigned DiagID = isa<NamespaceDecl>(PrevDecl) ? diag::err_redefinition :
      diag::err_redefinition_different_kind;
    Diag(AliasLoc, DiagID) << Alias;
    Diag(PrevDecl->getLocation(), diag::note_previous_definition);
    return DeclPtrTy();
  }

  if (R.isAmbiguous())
    return DeclPtrTy();

  if (R.empty()) {
    Diag(NamespaceLoc, diag::err_expected_namespace_name) << SS.getRange();
    return DeclPtrTy();
  }

  NamespaceAliasDecl *AliasDecl =
    NamespaceAliasDecl::Create(Context, CurContext, NamespaceLoc, AliasLoc,
                               Alias, SS.getRange(),
                               (NestedNameSpecifier *)SS.getScopeRep(),
                               IdentLoc, R.getFoundDecl());

  CurContext->addDecl(AliasDecl);
  return DeclPtrTy::make(AliasDecl);
}

void Sema::DefineImplicitDefaultConstructor(SourceLocation CurrentLocation,
                                            CXXConstructorDecl *Constructor) {
  assert((Constructor->isImplicit() && Constructor->isDefaultConstructor() &&
          !Constructor->isUsed()) &&
    "DefineImplicitDefaultConstructor - call it for implicit default ctor");

  CXXRecordDecl *ClassDecl
    = cast<CXXRecordDecl>(Constructor->getDeclContext());
  assert(ClassDecl && "DefineImplicitDefaultConstructor - invalid constructor");

  if (SetBaseOrMemberInitializers(Constructor, 0, 0, true)) {
    Diag(CurrentLocation, diag::note_member_synthesized_at) 
      << CXXDefaultConstructor << Context.getTagDeclType(ClassDecl);
    Constructor->setInvalidDecl();
  } else {
    Constructor->setUsed();
  }
}

void Sema::DefineImplicitDestructor(SourceLocation CurrentLocation,
                                    CXXDestructorDecl *Destructor) {
  assert((Destructor->isImplicit() && !Destructor->isUsed()) &&
         "DefineImplicitDestructor - call it for implicit default dtor");
  CXXRecordDecl *ClassDecl = Destructor->getParent();
  assert(ClassDecl && "DefineImplicitDestructor - invalid destructor");
  // C++ [class.dtor] p5
  // Before the implicitly-declared default destructor for a class is
  // implicitly defined, all the implicitly-declared default destructors
  // for its base class and its non-static data members shall have been
  // implicitly defined.
  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin(),
       E = ClassDecl->bases_end(); Base != E; ++Base) {
    CXXRecordDecl *BaseClassDecl
      = cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
    if (!BaseClassDecl->hasTrivialDestructor()) {
      if (CXXDestructorDecl *BaseDtor =
          const_cast<CXXDestructorDecl*>(BaseClassDecl->getDestructor(Context)))
        MarkDeclarationReferenced(CurrentLocation, BaseDtor);
      else
        assert(false &&
               "DefineImplicitDestructor - missing dtor in a base class");
    }
  }

  for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin(),
       E = ClassDecl->field_end(); Field != E; ++Field) {
    QualType FieldType = Context.getCanonicalType((*Field)->getType());
    if (const ArrayType *Array = Context.getAsArrayType(FieldType))
      FieldType = Array->getElementType();
    if (const RecordType *FieldClassType = FieldType->getAs<RecordType>()) {
      CXXRecordDecl *FieldClassDecl
        = cast<CXXRecordDecl>(FieldClassType->getDecl());
      if (!FieldClassDecl->hasTrivialDestructor()) {
        if (CXXDestructorDecl *FieldDtor =
            const_cast<CXXDestructorDecl*>(
                                        FieldClassDecl->getDestructor(Context)))
          MarkDeclarationReferenced(CurrentLocation, FieldDtor);
        else
          assert(false &&
          "DefineImplicitDestructor - missing dtor in class of a data member");
      }
    }
  }
  
  // FIXME: If CheckDestructor fails, we should emit a note about where the
  // implicit destructor was needed.
  if (CheckDestructor(Destructor)) {
    Diag(CurrentLocation, diag::note_member_synthesized_at) 
      << CXXDestructor << Context.getTagDeclType(ClassDecl);

    Destructor->setInvalidDecl();
    return;
  }

  Destructor->setUsed();
}

void Sema::DefineImplicitOverloadedAssign(SourceLocation CurrentLocation,
                                          CXXMethodDecl *MethodDecl) {
  assert((MethodDecl->isImplicit() && MethodDecl->isOverloadedOperator() &&
          MethodDecl->getOverloadedOperator() == OO_Equal &&
          !MethodDecl->isUsed()) &&
         "DefineImplicitOverloadedAssign - call it for implicit assignment op");

  CXXRecordDecl *ClassDecl
    = cast<CXXRecordDecl>(MethodDecl->getDeclContext());

  // C++[class.copy] p12
  // Before the implicitly-declared copy assignment operator for a class is
  // implicitly defined, all implicitly-declared copy assignment operators
  // for its direct base classes and its nonstatic data members shall have
  // been implicitly defined.
  bool err = false;
  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin(),
       E = ClassDecl->bases_end(); Base != E; ++Base) {
    CXXRecordDecl *BaseClassDecl
      = cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
    if (CXXMethodDecl *BaseAssignOpMethod =
          getAssignOperatorMethod(CurrentLocation, MethodDecl->getParamDecl(0), 
                                  BaseClassDecl))
      MarkDeclarationReferenced(CurrentLocation, BaseAssignOpMethod);
  }
  for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin(),
       E = ClassDecl->field_end(); Field != E; ++Field) {
    QualType FieldType = Context.getCanonicalType((*Field)->getType());
    if (const ArrayType *Array = Context.getAsArrayType(FieldType))
      FieldType = Array->getElementType();
    if (const RecordType *FieldClassType = FieldType->getAs<RecordType>()) {
      CXXRecordDecl *FieldClassDecl
        = cast<CXXRecordDecl>(FieldClassType->getDecl());
      if (CXXMethodDecl *FieldAssignOpMethod =
          getAssignOperatorMethod(CurrentLocation, MethodDecl->getParamDecl(0), 
                                  FieldClassDecl))
        MarkDeclarationReferenced(CurrentLocation, FieldAssignOpMethod);
    } else if (FieldType->isReferenceType()) {
      Diag(ClassDecl->getLocation(), diag::err_uninitialized_member_for_assign)
      << Context.getTagDeclType(ClassDecl) << 0 << Field->getDeclName();
      Diag(Field->getLocation(), diag::note_declared_at);
      Diag(CurrentLocation, diag::note_first_required_here);
      err = true;
    } else if (FieldType.isConstQualified()) {
      Diag(ClassDecl->getLocation(), diag::err_uninitialized_member_for_assign)
      << Context.getTagDeclType(ClassDecl) << 1 << Field->getDeclName();
      Diag(Field->getLocation(), diag::note_declared_at);
      Diag(CurrentLocation, diag::note_first_required_here);
      err = true;
    }
  }
  if (!err)
    MethodDecl->setUsed();
}

CXXMethodDecl *
Sema::getAssignOperatorMethod(SourceLocation CurrentLocation,
                              ParmVarDecl *ParmDecl,
                              CXXRecordDecl *ClassDecl) {
  QualType LHSType = Context.getTypeDeclType(ClassDecl);
  QualType RHSType(LHSType);
  // If class's assignment operator argument is const/volatile qualified,
  // look for operator = (const/volatile B&). Otherwise, look for
  // operator = (B&).
  RHSType = Context.getCVRQualifiedType(RHSType,
                                     ParmDecl->getType().getCVRQualifiers());
  ExprOwningPtr<Expr> LHS(this,  new (Context) DeclRefExpr(ParmDecl,
                                                           LHSType,
                                                           SourceLocation()));
  ExprOwningPtr<Expr> RHS(this,  new (Context) DeclRefExpr(ParmDecl,
                                                           RHSType,
                                                           CurrentLocation));
  Expr *Args[2] = { &*LHS, &*RHS };
  OverloadCandidateSet CandidateSet;
  AddMemberOperatorCandidates(clang::OO_Equal, SourceLocation(), Args, 2,
                              CandidateSet);
  OverloadCandidateSet::iterator Best;
  if (BestViableFunction(CandidateSet, CurrentLocation, Best) == OR_Success)
    return cast<CXXMethodDecl>(Best->Function);
  assert(false &&
         "getAssignOperatorMethod - copy assignment operator method not found");
  return 0;
}

void Sema::DefineImplicitCopyConstructor(SourceLocation CurrentLocation,
                                   CXXConstructorDecl *CopyConstructor,
                                   unsigned TypeQuals) {
  assert((CopyConstructor->isImplicit() &&
          CopyConstructor->isCopyConstructor(TypeQuals) &&
          !CopyConstructor->isUsed()) &&
         "DefineImplicitCopyConstructor - call it for implicit copy ctor");

  CXXRecordDecl *ClassDecl
    = cast<CXXRecordDecl>(CopyConstructor->getDeclContext());
  assert(ClassDecl && "DefineImplicitCopyConstructor - invalid constructor");
  // C++ [class.copy] p209
  // Before the implicitly-declared copy constructor for a class is
  // implicitly defined, all the implicitly-declared copy constructors
  // for its base class and its non-static data members shall have been
  // implicitly defined.
  for (CXXRecordDecl::base_class_iterator Base = ClassDecl->bases_begin();
       Base != ClassDecl->bases_end(); ++Base) {
    CXXRecordDecl *BaseClassDecl
      = cast<CXXRecordDecl>(Base->getType()->getAs<RecordType>()->getDecl());
    if (CXXConstructorDecl *BaseCopyCtor =
        BaseClassDecl->getCopyConstructor(Context, TypeQuals))
      MarkDeclarationReferenced(CurrentLocation, BaseCopyCtor);
  }
  for (CXXRecordDecl::field_iterator Field = ClassDecl->field_begin(),
                                  FieldEnd = ClassDecl->field_end();
       Field != FieldEnd; ++Field) {
    QualType FieldType = Context.getCanonicalType((*Field)->getType());
    if (const ArrayType *Array = Context.getAsArrayType(FieldType))
      FieldType = Array->getElementType();
    if (const RecordType *FieldClassType = FieldType->getAs<RecordType>()) {
      CXXRecordDecl *FieldClassDecl
        = cast<CXXRecordDecl>(FieldClassType->getDecl());
      if (CXXConstructorDecl *FieldCopyCtor =
          FieldClassDecl->getCopyConstructor(Context, TypeQuals))
        MarkDeclarationReferenced(CurrentLocation, FieldCopyCtor);
    }
  }
  CopyConstructor->setUsed();
}

Sema::OwningExprResult
Sema::BuildCXXConstructExpr(SourceLocation ConstructLoc, QualType DeclInitType,
                            CXXConstructorDecl *Constructor,
                            MultiExprArg ExprArgs,
                            bool RequiresZeroInit) {
  bool Elidable = false;

  // C++ [class.copy]p15:
  //   Whenever a temporary class object is copied using a copy constructor, and
  //   this object and the copy have the same cv-unqualified type, an
  //   implementation is permitted to treat the original and the copy as two
  //   different ways of referring to the same object and not perform a copy at
  //   all, even if the class copy constructor or destructor have side effects.

  // FIXME: Is this enough?
  if (Constructor->isCopyConstructor()) {
    Expr *E = ((Expr **)ExprArgs.get())[0];
    if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E))
      if (ICE->getCastKind() == CastExpr::CK_NoOp)
        E = ICE->getSubExpr();
    if (CXXFunctionalCastExpr *FCE = dyn_cast<CXXFunctionalCastExpr>(E))
      E = FCE->getSubExpr();
    while (CXXBindTemporaryExpr *BE = dyn_cast<CXXBindTemporaryExpr>(E))
      E = BE->getSubExpr();
    if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E))
      if (ICE->getCastKind() == CastExpr::CK_NoOp)
        E = ICE->getSubExpr();

    if (CallExpr *CE = dyn_cast<CallExpr>(E))
      Elidable = !CE->getCallReturnType()->isReferenceType();
    else if (isa<CXXTemporaryObjectExpr>(E))
      Elidable = true;
    else if (isa<CXXConstructExpr>(E))
      Elidable = true;
  }

  return BuildCXXConstructExpr(ConstructLoc, DeclInitType, Constructor,
                               Elidable, move(ExprArgs), RequiresZeroInit);
}

/// BuildCXXConstructExpr - Creates a complete call to a constructor,
/// including handling of its default argument expressions.
Sema::OwningExprResult
Sema::BuildCXXConstructExpr(SourceLocation ConstructLoc, QualType DeclInitType,
                            CXXConstructorDecl *Constructor, bool Elidable,
                            MultiExprArg ExprArgs,
                            bool RequiresZeroInit) {
  unsigned NumExprs = ExprArgs.size();
  Expr **Exprs = (Expr **)ExprArgs.release();

  MarkDeclarationReferenced(ConstructLoc, Constructor);
  return Owned(CXXConstructExpr::Create(Context, DeclInitType, ConstructLoc,
                                        Constructor, Elidable, Exprs, NumExprs, 
                                        RequiresZeroInit));
}

Sema::OwningExprResult
Sema::BuildCXXTemporaryObjectExpr(CXXConstructorDecl *Constructor,
                                  QualType Ty,
                                  SourceLocation TyBeginLoc,
                                  MultiExprArg Args,
                                  SourceLocation RParenLoc) {
  unsigned NumExprs = Args.size();
  Expr **Exprs = (Expr **)Args.release();

  MarkDeclarationReferenced(TyBeginLoc, Constructor);
  return Owned(new (Context) CXXTemporaryObjectExpr(Context, Constructor, Ty, 
                                                    TyBeginLoc, Exprs,
                                                    NumExprs, RParenLoc));
}


bool Sema::InitializeVarWithConstructor(VarDecl *VD,
                                        CXXConstructorDecl *Constructor,
                                        MultiExprArg Exprs) {
  OwningExprResult TempResult =
    BuildCXXConstructExpr(VD->getLocation(), VD->getType(), Constructor,
                          move(Exprs));
  if (TempResult.isInvalid())
    return true;

  Expr *Temp = TempResult.takeAs<Expr>();
  MarkDeclarationReferenced(VD->getLocation(), Constructor);
  Temp = MaybeCreateCXXExprWithTemporaries(Temp);
  VD->setInit(Context, Temp);

  return false;
}

void Sema::FinalizeVarWithDestructor(VarDecl *VD, QualType DeclInitType) {
  CXXRecordDecl *ClassDecl = cast<CXXRecordDecl>(
                                  DeclInitType->getAs<RecordType>()->getDecl());
  if (!ClassDecl->hasTrivialDestructor())
    if (CXXDestructorDecl *Destructor =
        const_cast<CXXDestructorDecl*>(ClassDecl->getDestructor(Context)))
      MarkDeclarationReferenced(VD->getLocation(), Destructor);
}

/// AddCXXDirectInitializerToDecl - This action is called immediately after
/// ActOnDeclarator, when a C++ direct initializer is present.
/// e.g: "int x(1);"
void Sema::AddCXXDirectInitializerToDecl(DeclPtrTy Dcl,
                                         SourceLocation LParenLoc,
                                         MultiExprArg Exprs,
                                         SourceLocation *CommaLocs,
                                         SourceLocation RParenLoc) {
  assert(Exprs.size() != 0 && Exprs.get() && "missing expressions");
  Decl *RealDecl = Dcl.getAs<Decl>();

  // If there is no declaration, there was an error parsing it.  Just ignore
  // the initializer.
  if (RealDecl == 0)
    return;

  VarDecl *VDecl = dyn_cast<VarDecl>(RealDecl);
  if (!VDecl) {
    Diag(RealDecl->getLocation(), diag::err_illegal_initializer);
    RealDecl->setInvalidDecl();
    return;
  }

  // We will represent direct-initialization similarly to copy-initialization:
  //    int x(1);  -as-> int x = 1;
  //    ClassType x(a,b,c); -as-> ClassType x = ClassType(a,b,c);
  //
  // Clients that want to distinguish between the two forms, can check for
  // direct initializer using VarDecl::hasCXXDirectInitializer().
  // A major benefit is that clients that don't particularly care about which
  // exactly form was it (like the CodeGen) can handle both cases without
  // special case code.

  // If either the declaration has a dependent type or if any of the expressions
  // is type-dependent, we represent the initialization via a ParenListExpr for
  // later use during template instantiation.
  if (VDecl->getType()->isDependentType() ||
      Expr::hasAnyTypeDependentArguments((Expr **)Exprs.get(), Exprs.size())) {
    // Let clients know that initialization was done with a direct initializer.
    VDecl->setCXXDirectInitializer(true);

    // Store the initialization expressions as a ParenListExpr.
    unsigned NumExprs = Exprs.size();
    VDecl->setInit(Context,
                   new (Context) ParenListExpr(Context, LParenLoc,
                                               (Expr **)Exprs.release(),
                                               NumExprs, RParenLoc));
    return;
  }


  // C++ 8.5p11:
  // The form of initialization (using parentheses or '=') is generally
  // insignificant, but does matter when the entity being initialized has a
  // class type.
  QualType DeclInitType = VDecl->getType();
  if (const ArrayType *Array = Context.getAsArrayType(DeclInitType))
    DeclInitType = Context.getBaseElementType(Array);

  if (RequireCompleteType(VDecl->getLocation(), VDecl->getType(),
                          diag::err_typecheck_decl_incomplete_type)) {
    VDecl->setInvalidDecl();
    return;
  }

  // The variable can not have an abstract class type.
  if (RequireNonAbstractType(VDecl->getLocation(), VDecl->getType(),
                             diag::err_abstract_type_in_decl,
                             AbstractVariableType))
    VDecl->setInvalidDecl();

  const VarDecl *Def = 0;
  if (VDecl->getDefinition(Def)) {
    Diag(VDecl->getLocation(), diag::err_redefinition)
    << VDecl->getDeclName();
    Diag(Def->getLocation(), diag::note_previous_definition);
    VDecl->setInvalidDecl();
    return;
  }
  
  // Capture the variable that is being initialized and the style of
  // initialization.
  InitializedEntity Entity = InitializedEntity::InitializeVariable(VDecl);
  
  // FIXME: Poor source location information.
  InitializationKind Kind
    = InitializationKind::CreateDirect(VDecl->getLocation(),
                                       LParenLoc, RParenLoc);
  
  InitializationSequence InitSeq(*this, Entity, Kind, 
                                 (Expr**)Exprs.get(), Exprs.size());
  OwningExprResult Result = InitSeq.Perform(*this, Entity, Kind, move(Exprs));
  if (Result.isInvalid()) {
    VDecl->setInvalidDecl();
    return;
  }
  
  Result = MaybeCreateCXXExprWithTemporaries(move(Result));
  VDecl->setInit(Context, Result.takeAs<Expr>());
  VDecl->setCXXDirectInitializer(true);

  if (VDecl->getType()->getAs<RecordType>())
    FinalizeVarWithDestructor(VDecl, DeclInitType);
}

/// \brief Add the applicable constructor candidates for an initialization
/// by constructor.
static void AddConstructorInitializationCandidates(Sema &SemaRef,
                                                   QualType ClassType,
                                                   Expr **Args,
                                                   unsigned NumArgs,
                                                   InitializationKind Kind,
                                           OverloadCandidateSet &CandidateSet) {
  // C++ [dcl.init]p14:
  //   If the initialization is direct-initialization, or if it is
  //   copy-initialization where the cv-unqualified version of the
  //   source type is the same class as, or a derived class of, the
  //   class of the destination, constructors are considered. The
  //   applicable constructors are enumerated (13.3.1.3), and the
  //   best one is chosen through overload resolution (13.3). The
  //   constructor so selected is called to initialize the object,
  //   with the initializer expression(s) as its argument(s). If no
  //   constructor applies, or the overload resolution is ambiguous,
  //   the initialization is ill-formed.
  const RecordType *ClassRec = ClassType->getAs<RecordType>();
  assert(ClassRec && "Can only initialize a class type here");
  
  // FIXME: When we decide not to synthesize the implicitly-declared
  // constructors, we'll need to make them appear here.

  const CXXRecordDecl *ClassDecl = cast<CXXRecordDecl>(ClassRec->getDecl());
  DeclarationName ConstructorName
    = SemaRef.Context.DeclarationNames.getCXXConstructorName(
              SemaRef.Context.getCanonicalType(ClassType).getUnqualifiedType());
  DeclContext::lookup_const_iterator Con, ConEnd;
  for (llvm::tie(Con, ConEnd) = ClassDecl->lookup(ConstructorName);
       Con != ConEnd; ++Con) {
    // Find the constructor (which may be a template).
    CXXConstructorDecl *Constructor = 0;
    FunctionTemplateDecl *ConstructorTmpl= dyn_cast<FunctionTemplateDecl>(*Con);
    if (ConstructorTmpl)
      Constructor
      = cast<CXXConstructorDecl>(ConstructorTmpl->getTemplatedDecl());
    else
      Constructor = cast<CXXConstructorDecl>(*Con);
    
    if ((Kind.getKind() == InitializationKind::IK_Direct) ||
        (Kind.getKind() == InitializationKind::IK_Value) ||
        (Kind.getKind() == InitializationKind::IK_Copy &&
         Constructor->isConvertingConstructor(/*AllowExplicit=*/false)) ||
        ((Kind.getKind() == InitializationKind::IK_Default) && 
         Constructor->isDefaultConstructor())) {
      if (ConstructorTmpl)
        SemaRef.AddTemplateOverloadCandidate(ConstructorTmpl,
                                             /*ExplicitArgs*/ 0,
                                             Args, NumArgs, CandidateSet);
      else
        SemaRef.AddOverloadCandidate(Constructor, Args, NumArgs, CandidateSet);
    }
  }
}

/// \brief Attempt to perform initialization by constructor 
/// (C++ [dcl.init]p14), which may occur as part of direct-initialization or 
/// copy-initialization. 
///
/// This routine determines whether initialization by constructor is possible,
/// but it does not emit any diagnostics in the case where the initialization
/// is ill-formed.
///
/// \param ClassType the type of the object being initialized, which must have
/// class type.
///
/// \param Args the arguments provided to initialize the object
///
/// \param NumArgs the number of arguments provided to initialize the object
///
/// \param Kind the type of initialization being performed
///
/// \returns the constructor used to initialize the object, if successful.
/// Otherwise, emits a diagnostic and returns NULL.
CXXConstructorDecl *
Sema::TryInitializationByConstructor(QualType ClassType,
                                     Expr **Args, unsigned NumArgs,
                                     SourceLocation Loc,
                                     InitializationKind Kind) {
  // Build the overload candidate set
  OverloadCandidateSet CandidateSet;
  AddConstructorInitializationCandidates(*this, ClassType, Args, NumArgs, Kind,
                                         CandidateSet);
  
  // Determine whether we found a constructor we can use.
  OverloadCandidateSet::iterator Best;
  switch (BestViableFunction(CandidateSet, Loc, Best)) {
    case OR_Success:
    case OR_Deleted:
      // We found a constructor. Return it.
      return cast<CXXConstructorDecl>(Best->Function);
      
    case OR_No_Viable_Function:
    case OR_Ambiguous:
      // Overload resolution failed. Return nothing.
      return 0;
  }
  
  // Silence GCC warning
  return 0;
}

/// \brief Perform initialization by constructor (C++ [dcl.init]p14), which 
/// may occur as part of direct-initialization or copy-initialization. 
///
/// \param ClassType the type of the object being initialized, which must have
/// class type.
///
/// \param ArgsPtr the arguments provided to initialize the object
///
/// \param Loc the source location where the initialization occurs
///
/// \param Range the source range that covers the entire initialization
///
/// \param InitEntity the name of the entity being initialized, if known
///
/// \param Kind the type of initialization being performed
///
/// \param ConvertedArgs a vector that will be filled in with the 
/// appropriately-converted arguments to the constructor (if initialization
/// succeeded).
///
/// \returns the constructor used to initialize the object, if successful.
/// Otherwise, emits a diagnostic and returns NULL.
CXXConstructorDecl *
Sema::PerformInitializationByConstructor(QualType ClassType,
                                         MultiExprArg ArgsPtr,
                                         SourceLocation Loc, SourceRange Range,
                                         DeclarationName InitEntity,
                                         InitializationKind Kind,
                      ASTOwningVector<&ActionBase::DeleteExpr> &ConvertedArgs) {
  
  // Build the overload candidate set
  Expr **Args = (Expr **)ArgsPtr.get();
  unsigned NumArgs = ArgsPtr.size();
  OverloadCandidateSet CandidateSet;
  AddConstructorInitializationCandidates(*this, ClassType, Args, NumArgs, Kind,
                                         CandidateSet);

  OverloadCandidateSet::iterator Best;
  switch (BestViableFunction(CandidateSet, Loc, Best)) {
  case OR_Success:
    // We found a constructor. Break out so that we can convert the arguments 
    // appropriately.
    break;

  case OR_No_Viable_Function:
    if (InitEntity)
      Diag(Loc, diag::err_ovl_no_viable_function_in_init)
        << InitEntity << Range;
    else
      Diag(Loc, diag::err_ovl_no_viable_function_in_init)
        << ClassType << Range;
    PrintOverloadCandidates(CandidateSet, /*OnlyViable=*/false);
    return 0;

  case OR_Ambiguous:
    if (InitEntity)
      Diag(Loc, diag::err_ovl_ambiguous_init) << InitEntity << Range;
    else
      Diag(Loc, diag::err_ovl_ambiguous_init) << ClassType << Range;
    PrintOverloadCandidates(CandidateSet, /*OnlyViable=*/true);
    return 0;

  case OR_Deleted:
    if (InitEntity)
      Diag(Loc, diag::err_ovl_deleted_init)
        << Best->Function->isDeleted()
        << InitEntity << Range;
    else {
      const CXXRecordDecl *RD =
          cast<CXXRecordDecl>(ClassType->getAs<RecordType>()->getDecl());
      Diag(Loc, diag::err_ovl_deleted_init)
        << Best->Function->isDeleted()
        << RD->getDeclName() << Range;
    }
    PrintOverloadCandidates(CandidateSet, /*OnlyViable=*/true);
    return 0;
  }

  // Convert the arguments, fill in default arguments, etc.
  CXXConstructorDecl *Constructor = cast<CXXConstructorDecl>(Best->Function);
  if (CompleteConstructorCall(Constructor, move(ArgsPtr), Loc, ConvertedArgs))
    return 0;
  
  return Constructor;
}

/// \brief Given a constructor and the set of arguments provided for the
/// constructor, convert the arguments and add any required default arguments
/// to form a proper call to this constructor.
///
/// \returns true if an error occurred, false otherwise.
bool 
Sema::CompleteConstructorCall(CXXConstructorDecl *Constructor,
                              MultiExprArg ArgsPtr,
                              SourceLocation Loc,                                    
                     ASTOwningVector<&ActionBase::DeleteExpr> &ConvertedArgs) {
  // FIXME: This duplicates a lot of code from Sema::ConvertArgumentsForCall.
  unsigned NumArgs = ArgsPtr.size();
  Expr **Args = (Expr **)ArgsPtr.get();

  const FunctionProtoType *Proto 
    = Constructor->getType()->getAs<FunctionProtoType>();
  assert(Proto && "Constructor without a prototype?");
  unsigned NumArgsInProto = Proto->getNumArgs();
  
  // If too few arguments are available, we'll fill in the rest with defaults.
  if (NumArgs < NumArgsInProto)
    ConvertedArgs.reserve(NumArgsInProto);
  else
    ConvertedArgs.reserve(NumArgs);

  VariadicCallType CallType = 
    Proto->isVariadic() ? VariadicConstructor : VariadicDoesNotApply;
  llvm::SmallVector<Expr *, 8> AllArgs;
  bool Invalid = GatherArgumentsForCall(Loc, Constructor,
                                        Proto, 0, Args, NumArgs, AllArgs, 
                                        CallType);
  for (unsigned i =0, size = AllArgs.size(); i < size; i++)
    ConvertedArgs.push_back(AllArgs[i]);
  return Invalid;
}

/// CompareReferenceRelationship - Compare the two types T1 and T2 to
/// determine whether they are reference-related,
/// reference-compatible, reference-compatible with added
/// qualification, or incompatible, for use in C++ initialization by
/// reference (C++ [dcl.ref.init]p4). Neither type can be a reference
/// type, and the first type (T1) is the pointee type of the reference
/// type being initialized.
Sema::ReferenceCompareResult
Sema::CompareReferenceRelationship(SourceLocation Loc, 
                                   QualType OrigT1, QualType OrigT2,
                                   bool& DerivedToBase) {
  assert(!OrigT1->isReferenceType() &&
    "T1 must be the pointee type of the reference type");
  assert(!OrigT2->isReferenceType() && "T2 cannot be a reference type");

  QualType T1 = Context.getCanonicalType(OrigT1);
  QualType T2 = Context.getCanonicalType(OrigT2);
  QualType UnqualT1 = T1.getLocalUnqualifiedType();
  QualType UnqualT2 = T2.getLocalUnqualifiedType();

  // C++ [dcl.init.ref]p4:
  //   Given types "cv1 T1" and "cv2 T2," "cv1 T1" is
  //   reference-related to "cv2 T2" if T1 is the same type as T2, or
  //   T1 is a base class of T2.
  if (UnqualT1 == UnqualT2)
    DerivedToBase = false;
  else if (!RequireCompleteType(Loc, OrigT1, PDiag()) &&
           !RequireCompleteType(Loc, OrigT2, PDiag()) &&
           IsDerivedFrom(UnqualT2, UnqualT1))
    DerivedToBase = true;
  else
    return Ref_Incompatible;

  // At this point, we know that T1 and T2 are reference-related (at
  // least).

  // C++ [dcl.init.ref]p4:
  //   "cv1 T1" is reference-compatible with "cv2 T2" if T1 is
  //   reference-related to T2 and cv1 is the same cv-qualification
  //   as, or greater cv-qualification than, cv2. For purposes of
  //   overload resolution, cases for which cv1 is greater
  //   cv-qualification than cv2 are identified as
  //   reference-compatible with added qualification (see 13.3.3.2).
  if (T1.getCVRQualifiers() == T2.getCVRQualifiers())
    return Ref_Compatible;
  else if (T1.isMoreQualifiedThan(T2))
    return Ref_Compatible_With_Added_Qualification;
  else
    return Ref_Related;
}

/// CheckReferenceInit - Check the initialization of a reference
/// variable with the given initializer (C++ [dcl.init.ref]). Init is
/// the initializer (either a simple initializer or an initializer
/// list), and DeclType is the type of the declaration. When ICS is
/// non-null, this routine will compute the implicit conversion
/// sequence according to C++ [over.ics.ref] and will not produce any
/// diagnostics; when ICS is null, it will emit diagnostics when any
/// errors are found. Either way, a return value of true indicates
/// that there was a failure, a return value of false indicates that
/// the reference initialization succeeded.
///
/// When @p SuppressUserConversions, user-defined conversions are
/// suppressed.
/// When @p AllowExplicit, we also permit explicit user-defined
/// conversion functions.
/// When @p ForceRValue, we unconditionally treat the initializer as an rvalue.
/// When @p IgnoreBaseAccess, we don't do access control on to-base conversion.
/// This is used when this is called from a C-style cast.
bool
Sema::CheckReferenceInit(Expr *&Init, QualType DeclType,
                         SourceLocation DeclLoc,
                         bool SuppressUserConversions,
                         bool AllowExplicit, bool ForceRValue,
                         ImplicitConversionSequence *ICS,
                         bool IgnoreBaseAccess) {
  assert(DeclType->isReferenceType() && "Reference init needs a reference");

  QualType T1 = DeclType->getAs<ReferenceType>()->getPointeeType();
  QualType T2 = Init->getType();

  // If the initializer is the address of an overloaded function, try
  // to resolve the overloaded function. If all goes well, T2 is the
  // type of the resulting function.
  if (Context.getCanonicalType(T2) == Context.OverloadTy) {
    FunctionDecl *Fn = ResolveAddressOfOverloadedFunction(Init, DeclType,
                                                          ICS != 0);
    if (Fn) {
      // Since we're performing this reference-initialization for
      // real, update the initializer with the resulting function.
      if (!ICS) {
        if (DiagnoseUseOfDecl(Fn, DeclLoc))
          return true; 

        Init = FixOverloadedFunctionReference(Init, Fn);
      }

      T2 = Fn->getType();
    }
  }

  // Compute some basic properties of the types and the initializer.
  bool isRValRef = DeclType->isRValueReferenceType();
  bool DerivedToBase = false;
  Expr::isLvalueResult InitLvalue = ForceRValue ? Expr::LV_InvalidExpression :
                                                  Init->isLvalue(Context);
  ReferenceCompareResult RefRelationship
    = CompareReferenceRelationship(DeclLoc, T1, T2, DerivedToBase);

  // Most paths end in a failed conversion.
  if (ICS)
    ICS->ConversionKind = ImplicitConversionSequence::BadConversion;

  // C++ [dcl.init.ref]p5:
  //   A reference to type "cv1 T1" is initialized by an expression
  //   of type "cv2 T2" as follows:

  //     -- If the initializer expression

  // Rvalue references cannot bind to lvalues (N2812).
  // There is absolutely no situation where they can. In particular, note that
  // this is ill-formed, even if B has a user-defined conversion to A&&:
  //   B b;
  //   A&& r = b;
  if (isRValRef && InitLvalue == Expr::LV_Valid) {
    if (!ICS)
      Diag(DeclLoc, diag::err_lvalue_to_rvalue_ref)
        << Init->getSourceRange();
    return true;
  }

  bool BindsDirectly = false;
  //       -- is an lvalue (but is not a bit-field), and "cv1 T1" is
  //          reference-compatible with "cv2 T2," or
  //
  // Note that the bit-field check is skipped if we are just computing
  // the implicit conversion sequence (C++ [over.best.ics]p2).
  if (InitLvalue == Expr::LV_Valid && (ICS || !Init->getBitField()) &&
      RefRelationship >= Ref_Compatible_With_Added_Qualification) {
    BindsDirectly = true;

    if (ICS) {
      // C++ [over.ics.ref]p1:
      //   When a parameter of reference type binds directly (8.5.3)
      //   to an argument expression, the implicit conversion sequence
      //   is the identity conversion, unless the argument expression
      //   has a type that is a derived class of the parameter type,
      //   in which case the implicit conversion sequence is a
      //   derived-to-base Conversion (13.3.3.1).
      ICS->ConversionKind = ImplicitConversionSequence::StandardConversion;
      ICS->Standard.First = ICK_Identity;
      ICS->Standard.Second = DerivedToBase? ICK_Derived_To_Base : ICK_Identity;
      ICS->Standard.Third = ICK_Identity;
      ICS->Standard.FromTypePtr = T2.getAsOpaquePtr();
      ICS->Standard.ToTypePtr = T1.getAsOpaquePtr();
      ICS->Standard.ReferenceBinding = true;
      ICS->Standard.DirectBinding = true;
      ICS->Standard.RRefBinding = false;
      ICS->Standard.CopyConstructor = 0;

      // Nothing more to do: the inaccessibility/ambiguity check for
      // derived-to-base conversions is suppressed when we're
      // computing the implicit conversion sequence (C++
      // [over.best.ics]p2).
      return false;
    } else {
      // Perform the conversion.
      CastExpr::CastKind CK = CastExpr::CK_NoOp;
      if (DerivedToBase)
        CK = CastExpr::CK_DerivedToBase;
      else if(CheckExceptionSpecCompatibility(Init, T1))
        return true;
      ImpCastExprToType(Init, T1, CK, /*isLvalue=*/true);
    }
  }

  //       -- has a class type (i.e., T2 is a class type) and can be
  //          implicitly converted to an lvalue of type "cv3 T3,"
  //          where "cv1 T1" is reference-compatible with "cv3 T3"
  //          92) (this conversion is selected by enumerating the
  //          applicable conversion functions (13.3.1.6) and choosing
  //          the best one through overload resolution (13.3)),
  if (!isRValRef && !SuppressUserConversions && T2->isRecordType() &&
      !RequireCompleteType(DeclLoc, T2, 0)) {
    CXXRecordDecl *T2RecordDecl
      = dyn_cast<CXXRecordDecl>(T2->getAs<RecordType>()->getDecl());

    OverloadCandidateSet CandidateSet;
    const UnresolvedSet *Conversions
      = T2RecordDecl->getVisibleConversionFunctions();
    for (UnresolvedSet::iterator I = Conversions->begin(),
           E = Conversions->end(); I != E; ++I) {
      NamedDecl *D = *I;
      CXXRecordDecl *ActingDC = cast<CXXRecordDecl>(D->getDeclContext());
      if (isa<UsingShadowDecl>(D))
        D = cast<UsingShadowDecl>(D)->getTargetDecl();

      FunctionTemplateDecl *ConvTemplate
        = dyn_cast<FunctionTemplateDecl>(D);
      CXXConversionDecl *Conv;
      if (ConvTemplate)
        Conv = cast<CXXConversionDecl>(ConvTemplate->getTemplatedDecl());
      else
        Conv = cast<CXXConversionDecl>(D);
      
      // If the conversion function doesn't return a reference type,
      // it can't be considered for this conversion.
      if (Conv->getConversionType()->isLValueReferenceType() &&
          (AllowExplicit || !Conv->isExplicit())) {
        if (ConvTemplate)
          AddTemplateConversionCandidate(ConvTemplate, ActingDC,
                                         Init, DeclType, CandidateSet);
        else
          AddConversionCandidate(Conv, ActingDC, Init, DeclType, CandidateSet);
      }
    }

    OverloadCandidateSet::iterator Best;
    switch (BestViableFunction(CandidateSet, DeclLoc, Best)) {
    case OR_Success:
      // This is a direct binding.
      BindsDirectly = true;

      if (ICS) {
        // C++ [over.ics.ref]p1:
        //
        //   [...] If the parameter binds directly to the result of
        //   applying a conversion function to the argument
        //   expression, the implicit conversion sequence is a
        //   user-defined conversion sequence (13.3.3.1.2), with the
        //   second standard conversion sequence either an identity
        //   conversion or, if the conversion function returns an
        //   entity of a type that is a derived class of the parameter
        //   type, a derived-to-base Conversion.
        ICS->ConversionKind = ImplicitConversionSequence::UserDefinedConversion;
        ICS->UserDefined.Before = Best->Conversions[0].Standard;
        ICS->UserDefined.After = Best->FinalConversion;
        ICS->UserDefined.ConversionFunction = Best->Function;
        ICS->UserDefined.EllipsisConversion = false;
        assert(ICS->UserDefined.After.ReferenceBinding &&
               ICS->UserDefined.After.DirectBinding &&
               "Expected a direct reference binding!");
        return false;
      } else {
        OwningExprResult InitConversion =
          BuildCXXCastArgument(DeclLoc, QualType(),
                               CastExpr::CK_UserDefinedConversion,
                               cast<CXXMethodDecl>(Best->Function), 
                               Owned(Init));
        Init = InitConversion.takeAs<Expr>();

        if (CheckExceptionSpecCompatibility(Init, T1))
          return true;
        ImpCastExprToType(Init, T1, CastExpr::CK_UserDefinedConversion, 
                          /*isLvalue=*/true);
      }
      break;

    case OR_Ambiguous:
      if (ICS) {
        for (OverloadCandidateSet::iterator Cand = CandidateSet.begin();
             Cand != CandidateSet.end(); ++Cand)
          if (Cand->Viable)
            ICS->ConversionFunctionSet.push_back(Cand->Function);
        break;
      }
      Diag(DeclLoc, diag::err_ref_init_ambiguous) << DeclType << Init->getType()
            << Init->getSourceRange();
      PrintOverloadCandidates(CandidateSet, /*OnlyViable=*/true);
      return true;

    case OR_No_Viable_Function:
    case OR_Deleted:
      // There was no suitable conversion, or we found a deleted
      // conversion; continue with other checks.
      break;
    }
  }

  if (BindsDirectly) {
    // C++ [dcl.init.ref]p4:
    //   [...] In all cases where the reference-related or
    //   reference-compatible relationship of two types is used to
    //   establish the validity of a reference binding, and T1 is a
    //   base class of T2, a program that necessitates such a binding
    //   is ill-formed if T1 is an inaccessible (clause 11) or
    //   ambiguous (10.2) base class of T2.
    //
    // Note that we only check this condition when we're allowed to
    // complain about errors, because we should not be checking for
    // ambiguity (or inaccessibility) unless the reference binding
    // actually happens.
    if (DerivedToBase)
      return CheckDerivedToBaseConversion(T2, T1, DeclLoc,
                                          Init->getSourceRange(),
                                          IgnoreBaseAccess);
    else
      return false;
  }

  //     -- Otherwise, the reference shall be to a non-volatile const
  //        type (i.e., cv1 shall be const), or the reference shall be an
  //        rvalue reference and the initializer expression shall be an rvalue.
  if (!isRValRef && T1.getCVRQualifiers() != Qualifiers::Const) {
    if (!ICS)
      Diag(DeclLoc, diag::err_not_reference_to_const_init)
        << T1 << int(InitLvalue != Expr::LV_Valid)
        << T2 << Init->getSourceRange();
    return true;
  }

  //       -- If the initializer expression is an rvalue, with T2 a
  //          class type, and "cv1 T1" is reference-compatible with
  //          "cv2 T2," the reference is bound in one of the
  //          following ways (the choice is implementation-defined):
  //
  //          -- The reference is bound to the object represented by
  //             the rvalue (see 3.10) or to a sub-object within that
  //             object.
  //
  //          -- A temporary of type "cv1 T2" [sic] is created, and
  //             a constructor is called to copy the entire rvalue
  //             object into the temporary. The reference is bound to
  //             the temporary or to a sub-object within the
  //             temporary.
  //
  //          The constructor that would be used to make the copy
  //          shall be callable whether or not the copy is actually
  //          done.
  //
  // Note that C++0x [dcl.init.ref]p5 takes away this implementation
  // freedom, so we will always take the first option and never build
  // a temporary in this case. FIXME: We will, however, have to check
  // for the presence of a copy constructor in C++98/03 mode.
  if (InitLvalue != Expr::LV_Valid && T2->isRecordType() &&
      RefRelationship >= Ref_Compatible_With_Added_Qualification) {
    if (ICS) {
      ICS->ConversionKind = ImplicitConversionSequence::StandardConversion;
      ICS->Standard.First = ICK_Identity;
      ICS->Standard.Second = DerivedToBase? ICK_Derived_To_Base : ICK_Identity;
      ICS->Standard.Third = ICK_Identity;
      ICS->Standard.FromTypePtr = T2.getAsOpaquePtr();
      ICS->Standard.ToTypePtr = T1.getAsOpaquePtr();
      ICS->Standard.ReferenceBinding = true;
      ICS->Standard.DirectBinding = false;
      ICS->Standard.RRefBinding = isRValRef;
      ICS->Standard.CopyConstructor = 0;
    } else {
      CastExpr::CastKind CK = CastExpr::CK_NoOp;
      if (DerivedToBase)
        CK = CastExpr::CK_DerivedToBase;
      else if(CheckExceptionSpecCompatibility(Init, T1))
        return true;
      ImpCastExprToType(Init, T1, CK, /*isLvalue=*/false);
    }
    return false;
  }

  //       -- Otherwise, a temporary of type "cv1 T1" is created and
  //          initialized from the initializer expression using the
  //          rules for a non-reference copy initialization (8.5). The
  //          reference is then bound to the temporary. If T1 is
  //          reference-related to T2, cv1 must be the same
  //          cv-qualification as, or greater cv-qualification than,
  //          cv2; otherwise, the program is ill-formed.
  if (RefRelationship == Ref_Related) {
    // If cv1 == cv2 or cv1 is a greater cv-qualified than cv2, then
    // we would be reference-compatible or reference-compatible with
    // added qualification. But that wasn't the case, so the reference
    // initialization fails.
    if (!ICS)
      Diag(DeclLoc, diag::err_reference_init_drops_quals)
        << T1 << int(InitLvalue != Expr::LV_Valid)
        << T2 << Init->getSourceRange();
    return true;
  }

  // If at least one of the types is a class type, the types are not
  // related, and we aren't allowed any user conversions, the
  // reference binding fails. This case is important for breaking
  // recursion, since TryImplicitConversion below will attempt to
  // create a temporary through the use of a copy constructor.
  if (SuppressUserConversions && RefRelationship == Ref_Incompatible &&
      (T1->isRecordType() || T2->isRecordType())) {
    if (!ICS)
      Diag(DeclLoc, diag::err_typecheck_convert_incompatible)
        << DeclType << Init->getType() << AA_Initializing << Init->getSourceRange();
    return true;
  }

  // Actually try to convert the initializer to T1.
  if (ICS) {
    // C++ [over.ics.ref]p2:
    //
    //   When a parameter of reference type is not bound directly to
    //   an argument expression, the conversion sequence is the one
    //   required to convert the argument expression to the
    //   underlying type of the reference according to
    //   13.3.3.1. Conceptually, this conversion sequence corresponds
    //   to copy-initializing a temporary of the underlying type with
    //   the argument expression. Any difference in top-level
    //   cv-qualification is subsumed by the initialization itself
    //   and does not constitute a conversion.
    *ICS = TryImplicitConversion(Init, T1, SuppressUserConversions,
                                 /*AllowExplicit=*/false,
                                 /*ForceRValue=*/false,
                                 /*InOverloadResolution=*/false);

    // Of course, that's still a reference binding.
    if (ICS->ConversionKind == ImplicitConversionSequence::StandardConversion) {
      ICS->Standard.ReferenceBinding = true;
      ICS->Standard.RRefBinding = isRValRef;
    } else if (ICS->ConversionKind ==
              ImplicitConversionSequence::UserDefinedConversion) {
      ICS->UserDefined.After.ReferenceBinding = true;
      ICS->UserDefined.After.RRefBinding = isRValRef;
    }
    return ICS->ConversionKind == ImplicitConversionSequence::BadConversion;
  } else {
    ImplicitConversionSequence Conversions;
    bool badConversion = PerformImplicitConversion(Init, T1, AA_Initializing, 
                                                   false, false, 
                                                   Conversions);
    if (badConversion) {
      if ((Conversions.ConversionKind  == 
            ImplicitConversionSequence::BadConversion)
          && !Conversions.ConversionFunctionSet.empty()) {
        Diag(DeclLoc, 
             diag::err_lvalue_to_rvalue_ambig_ref) << Init->getSourceRange();
        for (int j = Conversions.ConversionFunctionSet.size()-1; 
             j >= 0; j--) {
          FunctionDecl *Func = Conversions.ConversionFunctionSet[j];
          Diag(Func->getLocation(), diag::err_ovl_candidate);
        }
      }
      else {
        if (isRValRef)
          Diag(DeclLoc, diag::err_lvalue_to_rvalue_ref) 
            << Init->getSourceRange();
        else
          Diag(DeclLoc, diag::err_invalid_initialization)
            << DeclType << Init->getType() << Init->getSourceRange();
      }
    }
    return badConversion;
  }
}

static inline bool
CheckOperatorNewDeleteDeclarationScope(Sema &SemaRef, 
                                       const FunctionDecl *FnDecl) {
  const DeclContext *DC = FnDecl->getDeclContext()->getLookupContext();
  if (isa<NamespaceDecl>(DC)) {
    return SemaRef.Diag(FnDecl->getLocation(), 
                        diag::err_operator_new_delete_declared_in_namespace)
      << FnDecl->getDeclName();
  }
  
  if (isa<TranslationUnitDecl>(DC) && 
      FnDecl->getStorageClass() == FunctionDecl::Static) {
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_new_delete_declared_static)
      << FnDecl->getDeclName();
  }
  
  return false;
}

static inline bool
CheckOperatorNewDeleteTypes(Sema &SemaRef, const FunctionDecl *FnDecl,
                            CanQualType ExpectedResultType,
                            CanQualType ExpectedFirstParamType,
                            unsigned DependentParamTypeDiag,
                            unsigned InvalidParamTypeDiag) {
  QualType ResultType = 
    FnDecl->getType()->getAs<FunctionType>()->getResultType();

  // Check that the result type is not dependent.
  if (ResultType->isDependentType())
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_new_delete_dependent_result_type)
    << FnDecl->getDeclName() << ExpectedResultType;

  // Check that the result type is what we expect.
  if (SemaRef.Context.getCanonicalType(ResultType) != ExpectedResultType)
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_new_delete_invalid_result_type) 
    << FnDecl->getDeclName() << ExpectedResultType;
  
  // A function template must have at least 2 parameters.
  if (FnDecl->getDescribedFunctionTemplate() && FnDecl->getNumParams() < 2)
    return SemaRef.Diag(FnDecl->getLocation(),
                      diag::err_operator_new_delete_template_too_few_parameters)
        << FnDecl->getDeclName();
  
  // The function decl must have at least 1 parameter.
  if (FnDecl->getNumParams() == 0)
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_new_delete_too_few_parameters)
      << FnDecl->getDeclName();
 
  // Check the the first parameter type is not dependent.
  QualType FirstParamType = FnDecl->getParamDecl(0)->getType();
  if (FirstParamType->isDependentType())
    return SemaRef.Diag(FnDecl->getLocation(), DependentParamTypeDiag)
      << FnDecl->getDeclName() << ExpectedFirstParamType;

  // Check that the first parameter type is what we expect.
  if (SemaRef.Context.getCanonicalType(FirstParamType).getUnqualifiedType() != 
      ExpectedFirstParamType)
    return SemaRef.Diag(FnDecl->getLocation(), InvalidParamTypeDiag)
    << FnDecl->getDeclName() << ExpectedFirstParamType;
  
  return false;
}

static bool
CheckOperatorNewDeclaration(Sema &SemaRef, const FunctionDecl *FnDecl) {
  // C++ [basic.stc.dynamic.allocation]p1:
  //   A program is ill-formed if an allocation function is declared in a
  //   namespace scope other than global scope or declared static in global 
  //   scope.
  if (CheckOperatorNewDeleteDeclarationScope(SemaRef, FnDecl))
    return true;

  CanQualType SizeTy = 
    SemaRef.Context.getCanonicalType(SemaRef.Context.getSizeType());

  // C++ [basic.stc.dynamic.allocation]p1:
  //  The return type shall be void*. The first parameter shall have type 
  //  std::size_t.
  if (CheckOperatorNewDeleteTypes(SemaRef, FnDecl, SemaRef.Context.VoidPtrTy, 
                                  SizeTy,
                                  diag::err_operator_new_dependent_param_type,
                                  diag::err_operator_new_param_type))
    return true;

  // C++ [basic.stc.dynamic.allocation]p1:
  //  The first parameter shall not have an associated default argument.
  if (FnDecl->getParamDecl(0)->hasDefaultArg())
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_new_default_arg)
      << FnDecl->getDeclName() << FnDecl->getParamDecl(0)->getDefaultArgRange();

  return false;
}

static bool
CheckOperatorDeleteDeclaration(Sema &SemaRef, const FunctionDecl *FnDecl) {
  // C++ [basic.stc.dynamic.deallocation]p1:
  //   A program is ill-formed if deallocation functions are declared in a
  //   namespace scope other than global scope or declared static in global 
  //   scope.
  if (CheckOperatorNewDeleteDeclarationScope(SemaRef, FnDecl))
    return true;

  // C++ [basic.stc.dynamic.deallocation]p2:
  //   Each deallocation function shall return void and its first parameter 
  //   shall be void*.
  if (CheckOperatorNewDeleteTypes(SemaRef, FnDecl, SemaRef.Context.VoidTy, 
                                  SemaRef.Context.VoidPtrTy,
                                 diag::err_operator_delete_dependent_param_type,
                                 diag::err_operator_delete_param_type))
    return true;

  QualType FirstParamType = FnDecl->getParamDecl(0)->getType();
  if (FirstParamType->isDependentType())
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_delete_dependent_param_type)
    << FnDecl->getDeclName() << SemaRef.Context.VoidPtrTy;

  if (SemaRef.Context.getCanonicalType(FirstParamType) != 
      SemaRef.Context.VoidPtrTy)
    return SemaRef.Diag(FnDecl->getLocation(),
                        diag::err_operator_delete_param_type)
      << FnDecl->getDeclName() << SemaRef.Context.VoidPtrTy;
  
  return false;
}

/// CheckOverloadedOperatorDeclaration - Check whether the declaration
/// of this overloaded operator is well-formed. If so, returns false;
/// otherwise, emits appropriate diagnostics and returns true.
bool Sema::CheckOverloadedOperatorDeclaration(FunctionDecl *FnDecl) {
  assert(FnDecl && FnDecl->isOverloadedOperator() &&
         "Expected an overloaded operator declaration");

  OverloadedOperatorKind Op = FnDecl->getOverloadedOperator();

  // C++ [over.oper]p5:
  //   The allocation and deallocation functions, operator new,
  //   operator new[], operator delete and operator delete[], are
  //   described completely in 3.7.3. The attributes and restrictions
  //   found in the rest of this subclause do not apply to them unless
  //   explicitly stated in 3.7.3.
  if (Op == OO_Delete || Op == OO_Array_Delete)
    return CheckOperatorDeleteDeclaration(*this, FnDecl);
  
  if (Op == OO_New || Op == OO_Array_New)
    return CheckOperatorNewDeclaration(*this, FnDecl);

  // C++ [over.oper]p6:
  //   An operator function shall either be a non-static member
  //   function or be a non-member function and have at least one
  //   parameter whose type is a class, a reference to a class, an
  //   enumeration, or a reference to an enumeration.
  if (CXXMethodDecl *MethodDecl = dyn_cast<CXXMethodDecl>(FnDecl)) {
    if (MethodDecl->isStatic())
      return Diag(FnDecl->getLocation(),
                  diag::err_operator_overload_static) << FnDecl->getDeclName();
  } else {
    bool ClassOrEnumParam = false;
    for (FunctionDecl::param_iterator Param = FnDecl->param_begin(),
                                   ParamEnd = FnDecl->param_end();
         Param != ParamEnd; ++Param) {
      QualType ParamType = (*Param)->getType().getNonReferenceType();
      if (ParamType->isDependentType() || ParamType->isRecordType() ||
          ParamType->isEnumeralType()) {
        ClassOrEnumParam = true;
        break;
      }
    }

    if (!ClassOrEnumParam)
      return Diag(FnDecl->getLocation(),
                  diag::err_operator_overload_needs_class_or_enum)
        << FnDecl->getDeclName();
  }

  // C++ [over.oper]p8:
  //   An operator function cannot have default arguments (8.3.6),
  //   except where explicitly stated below.
  //
  // Only the function-call operator allows default arguments
  // (C++ [over.call]p1).
  if (Op != OO_Call) {
    for (FunctionDecl::param_iterator Param = FnDecl->param_begin();
         Param != FnDecl->param_end(); ++Param) {
      if ((*Param)->hasDefaultArg())
        return Diag((*Param)->getLocation(),
                    diag::err_operator_overload_default_arg)
          << FnDecl->getDeclName() << (*Param)->getDefaultArgRange();
    }
  }

  static const bool OperatorUses[NUM_OVERLOADED_OPERATORS][3] = {
    { false, false, false }
#define OVERLOADED_OPERATOR(Name,Spelling,Token,Unary,Binary,MemberOnly) \
    , { Unary, Binary, MemberOnly }
#include "clang/Basic/OperatorKinds.def"
  };

  bool CanBeUnaryOperator = OperatorUses[Op][0];
  bool CanBeBinaryOperator = OperatorUses[Op][1];
  bool MustBeMemberOperator = OperatorUses[Op][2];

  // C++ [over.oper]p8:
  //   [...] Operator functions cannot have more or fewer parameters
  //   than the number required for the corresponding operator, as
  //   described in the rest of this subclause.
  unsigned NumParams = FnDecl->getNumParams()
                     + (isa<CXXMethodDecl>(FnDecl)? 1 : 0);
  if (Op != OO_Call &&
      ((NumParams == 1 && !CanBeUnaryOperator) ||
       (NumParams == 2 && !CanBeBinaryOperator) ||
       (NumParams < 1) || (NumParams > 2))) {
    // We have the wrong number of parameters.
    unsigned ErrorKind;
    if (CanBeUnaryOperator && CanBeBinaryOperator) {
      ErrorKind = 2;  // 2 -> unary or binary.
    } else if (CanBeUnaryOperator) {
      ErrorKind = 0;  // 0 -> unary
    } else {
      assert(CanBeBinaryOperator &&
             "All non-call overloaded operators are unary or binary!");
      ErrorKind = 1;  // 1 -> binary
    }

    return Diag(FnDecl->getLocation(), diag::err_operator_overload_must_be)
      << FnDecl->getDeclName() << NumParams << ErrorKind;
  }

  // Overloaded operators other than operator() cannot be variadic.
  if (Op != OO_Call &&
      FnDecl->getType()->getAs<FunctionProtoType>()->isVariadic()) {
    return Diag(FnDecl->getLocation(), diag::err_operator_overload_variadic)
      << FnDecl->getDeclName();
  }

  // Some operators must be non-static member functions.
  if (MustBeMemberOperator && !isa<CXXMethodDecl>(FnDecl)) {
    return Diag(FnDecl->getLocation(),
                diag::err_operator_overload_must_be_member)
      << FnDecl->getDeclName();
  }

  // C++ [over.inc]p1:
  //   The user-defined function called operator++ implements the
  //   prefix and postfix ++ operator. If this function is a member
  //   function with no parameters, or a non-member function with one
  //   parameter of class or enumeration type, it defines the prefix
  //   increment operator ++ for objects of that type. If the function
  //   is a member function with one parameter (which shall be of type
  //   int) or a non-member function with two parameters (the second
  //   of which shall be of type int), it defines the postfix
  //   increment operator ++ for objects of that type.
  if ((Op == OO_PlusPlus || Op == OO_MinusMinus) && NumParams == 2) {
    ParmVarDecl *LastParam = FnDecl->getParamDecl(FnDecl->getNumParams() - 1);
    bool ParamIsInt = false;
    if (const BuiltinType *BT = LastParam->getType()->getAs<BuiltinType>())
      ParamIsInt = BT->getKind() == BuiltinType::Int;

    if (!ParamIsInt)
      return Diag(LastParam->getLocation(),
                  diag::err_operator_overload_post_incdec_must_be_int)
        << LastParam->getType() << (Op == OO_MinusMinus);
  }

  // Notify the class if it got an assignment operator.
  if (Op == OO_Equal) {
    // Would have returned earlier otherwise.
    assert(isa<CXXMethodDecl>(FnDecl) &&
      "Overloaded = not member, but not filtered.");
    CXXMethodDecl *Method = cast<CXXMethodDecl>(FnDecl);
    Method->getParent()->addedAssignmentOperator(Context, Method);
  }

  return false;
}

/// ActOnStartLinkageSpecification - Parsed the beginning of a C++
/// linkage specification, including the language and (if present)
/// the '{'. ExternLoc is the location of the 'extern', LangLoc is
/// the location of the language string literal, which is provided
/// by Lang/StrSize. LBraceLoc, if valid, provides the location of
/// the '{' brace. Otherwise, this linkage specification does not
/// have any braces.
Sema::DeclPtrTy Sema::ActOnStartLinkageSpecification(Scope *S,
                                                     SourceLocation ExternLoc,
                                                     SourceLocation LangLoc,
                                                     const char *Lang,
                                                     unsigned StrSize,
                                                     SourceLocation LBraceLoc) {
  LinkageSpecDecl::LanguageIDs Language;
  if (strncmp(Lang, "\"C\"", StrSize) == 0)
    Language = LinkageSpecDecl::lang_c;
  else if (strncmp(Lang, "\"C++\"", StrSize) == 0)
    Language = LinkageSpecDecl::lang_cxx;
  else {
    Diag(LangLoc, diag::err_bad_language);
    return DeclPtrTy();
  }

  // FIXME: Add all the various semantics of linkage specifications

  LinkageSpecDecl *D = LinkageSpecDecl::Create(Context, CurContext,
                                               LangLoc, Language,
                                               LBraceLoc.isValid());
  CurContext->addDecl(D);
  PushDeclContext(S, D);
  return DeclPtrTy::make(D);
}

/// ActOnFinishLinkageSpecification - Completely the definition of
/// the C++ linkage specification LinkageSpec. If RBraceLoc is
/// valid, it's the position of the closing '}' brace in a linkage
/// specification that uses braces.
Sema::DeclPtrTy Sema::ActOnFinishLinkageSpecification(Scope *S,
                                                      DeclPtrTy LinkageSpec,
                                                      SourceLocation RBraceLoc) {
  if (LinkageSpec)
    PopDeclContext();
  return LinkageSpec;
}

/// \brief Perform semantic analysis for the variable declaration that
/// occurs within a C++ catch clause, returning the newly-created
/// variable.
VarDecl *Sema::BuildExceptionDeclaration(Scope *S, QualType ExDeclType,
                                         TypeSourceInfo *TInfo,
                                         IdentifierInfo *Name,
                                         SourceLocation Loc,
                                         SourceRange Range) {
  bool Invalid = false;

  // Arrays and functions decay.
  if (ExDeclType->isArrayType())
    ExDeclType = Context.getArrayDecayedType(ExDeclType);
  else if (ExDeclType->isFunctionType())
    ExDeclType = Context.getPointerType(ExDeclType);

  // C++ 15.3p1: The exception-declaration shall not denote an incomplete type.
  // The exception-declaration shall not denote a pointer or reference to an
  // incomplete type, other than [cv] void*.
  // N2844 forbids rvalue references.
  if (!ExDeclType->isDependentType() && ExDeclType->isRValueReferenceType()) {
    Diag(Loc, diag::err_catch_rvalue_ref) << Range;
    Invalid = true;
  }

  QualType BaseType = ExDeclType;
  int Mode = 0; // 0 for direct type, 1 for pointer, 2 for reference
  unsigned DK = diag::err_catch_incomplete;
  if (const PointerType *Ptr = BaseType->getAs<PointerType>()) {
    BaseType = Ptr->getPointeeType();
    Mode = 1;
    DK = diag::err_catch_incomplete_ptr;
  } else if (const ReferenceType *Ref = BaseType->getAs<ReferenceType>()) {
    // For the purpose of error recovery, we treat rvalue refs like lvalue refs.
    BaseType = Ref->getPointeeType();
    Mode = 2;
    DK = diag::err_catch_incomplete_ref;
  }
  if (!Invalid && (Mode == 0 || !BaseType->isVoidType()) &&
      !BaseType->isDependentType() && RequireCompleteType(Loc, BaseType, DK))
    Invalid = true;

  if (!Invalid && !ExDeclType->isDependentType() &&
      RequireNonAbstractType(Loc, ExDeclType,
                             diag::err_abstract_type_in_decl,
                             AbstractVariableType))
    Invalid = true;

  // FIXME: Need to test for ability to copy-construct and destroy the
  // exception variable.

  // FIXME: Need to check for abstract classes.

  VarDecl *ExDecl = VarDecl::Create(Context, CurContext, Loc,
                                    Name, ExDeclType, TInfo, VarDecl::None);

  if (Invalid)
    ExDecl->setInvalidDecl();

  return ExDecl;
}

/// ActOnExceptionDeclarator - Parsed the exception-declarator in a C++ catch
/// handler.
Sema::DeclPtrTy Sema::ActOnExceptionDeclarator(Scope *S, Declarator &D) {
  TypeSourceInfo *TInfo = 0;
  QualType ExDeclType = GetTypeForDeclarator(D, S, &TInfo);

  bool Invalid = D.isInvalidType();
  IdentifierInfo *II = D.getIdentifier();
  if (NamedDecl *PrevDecl = LookupSingleName(S, II, LookupOrdinaryName)) {
    // The scope should be freshly made just for us. There is just no way
    // it contains any previous declaration.
    assert(!S->isDeclScope(DeclPtrTy::make(PrevDecl)));
    if (PrevDecl->isTemplateParameter()) {
      // Maybe we will complain about the shadowed template parameter.
      DiagnoseTemplateParameterShadow(D.getIdentifierLoc(), PrevDecl);
    }
  }

  if (D.getCXXScopeSpec().isSet() && !Invalid) {
    Diag(D.getIdentifierLoc(), diag::err_qualified_catch_declarator)
      << D.getCXXScopeSpec().getRange();
    Invalid = true;
  }

  VarDecl *ExDecl = BuildExceptionDeclaration(S, ExDeclType, TInfo,
                                              D.getIdentifier(),
                                              D.getIdentifierLoc(),
                                            D.getDeclSpec().getSourceRange());

  if (Invalid)
    ExDecl->setInvalidDecl();

  // Add the exception declaration into this scope.
  if (II)
    PushOnScopeChains(ExDecl, S);
  else
    CurContext->addDecl(ExDecl);

  ProcessDeclAttributes(S, ExDecl, D);
  return DeclPtrTy::make(ExDecl);
}

Sema::DeclPtrTy Sema::ActOnStaticAssertDeclaration(SourceLocation AssertLoc,
                                                   ExprArg assertexpr,
                                                   ExprArg assertmessageexpr) {
  Expr *AssertExpr = (Expr *)assertexpr.get();
  StringLiteral *AssertMessage =
    cast<StringLiteral>((Expr *)assertmessageexpr.get());

  if (!AssertExpr->isTypeDependent() && !AssertExpr->isValueDependent()) {
    llvm::APSInt Value(32);
    if (!AssertExpr->isIntegerConstantExpr(Value, Context)) {
      Diag(AssertLoc, diag::err_static_assert_expression_is_not_constant) <<
        AssertExpr->getSourceRange();
      return DeclPtrTy();
    }

    if (Value == 0) {
      Diag(AssertLoc, diag::err_static_assert_failed)
        << AssertMessage->getString() << AssertExpr->getSourceRange();
    }
  }

  assertexpr.release();
  assertmessageexpr.release();
  Decl *Decl = StaticAssertDecl::Create(Context, CurContext, AssertLoc,
                                        AssertExpr, AssertMessage);

  CurContext->addDecl(Decl);
  return DeclPtrTy::make(Decl);
}

/// Handle a friend type declaration.  This works in tandem with
/// ActOnTag.
///
/// Notes on friend class templates:
///
/// We generally treat friend class declarations as if they were
/// declaring a class.  So, for example, the elaborated type specifier
/// in a friend declaration is required to obey the restrictions of a
/// class-head (i.e. no typedefs in the scope chain), template
/// parameters are required to match up with simple template-ids, &c.
/// However, unlike when declaring a template specialization, it's
/// okay to refer to a template specialization without an empty
/// template parameter declaration, e.g.
///   friend class A<T>::B<unsigned>;
/// We permit this as a special case; if there are any template
/// parameters present at all, require proper matching, i.e.
///   template <> template <class T> friend class A<int>::B;
Sema::DeclPtrTy Sema::ActOnFriendTypeDecl(Scope *S, const DeclSpec &DS,
                                          MultiTemplateParamsArg TempParams) {
  SourceLocation Loc = DS.getSourceRange().getBegin();

  assert(DS.isFriendSpecified());
  assert(DS.getStorageClassSpec() == DeclSpec::SCS_unspecified);

  // Try to convert the decl specifier to a type.  This works for
  // friend templates because ActOnTag never produces a ClassTemplateDecl
  // for a TUK_Friend.
  Declarator TheDeclarator(DS, Declarator::MemberContext);
  QualType T = GetTypeForDeclarator(TheDeclarator, S);
  if (TheDeclarator.isInvalidType())
    return DeclPtrTy();

  // This is definitely an error in C++98.  It's probably meant to
  // be forbidden in C++0x, too, but the specification is just
  // poorly written.
  //
  // The problem is with declarations like the following:
  //   template <T> friend A<T>::foo;
  // where deciding whether a class C is a friend or not now hinges
  // on whether there exists an instantiation of A that causes
  // 'foo' to equal C.  There are restrictions on class-heads
  // (which we declare (by fiat) elaborated friend declarations to
  // be) that makes this tractable.
  //
  // FIXME: handle "template <> friend class A<T>;", which
  // is possibly well-formed?  Who even knows?
  if (TempParams.size() && !isa<ElaboratedType>(T)) {
    Diag(Loc, diag::err_tagless_friend_type_template)
      << DS.getSourceRange();
    return DeclPtrTy();
  }

  // C++ [class.friend]p2:
  //   An elaborated-type-specifier shall be used in a friend declaration
  //   for a class.*
  //   * The class-key of the elaborated-type-specifier is required.
  // This is one of the rare places in Clang where it's legitimate to
  // ask about the "spelling" of the type.
  if (!getLangOptions().CPlusPlus0x && !isa<ElaboratedType>(T)) {
    // If we evaluated the type to a record type, suggest putting
    // a tag in front.
    if (const RecordType *RT = T->getAs<RecordType>()) {
      RecordDecl *RD = RT->getDecl();

      std::string InsertionText = std::string(" ") + RD->getKindName();

      Diag(DS.getTypeSpecTypeLoc(), diag::err_unelaborated_friend_type)
        << (unsigned) RD->getTagKind()
        << T
        << SourceRange(DS.getFriendSpecLoc())
        << CodeModificationHint::CreateInsertion(DS.getTypeSpecTypeLoc(),
                                                 InsertionText);
      return DeclPtrTy();
    }else {
      Diag(DS.getFriendSpecLoc(), diag::err_unexpected_friend)
          << DS.getSourceRange();
      return DeclPtrTy();
    }
  }

  // Enum types cannot be friends.
  if (T->getAs<EnumType>()) {
    Diag(DS.getTypeSpecTypeLoc(), diag::err_enum_friend)
      << SourceRange(DS.getFriendSpecLoc());
    return DeclPtrTy();
  }

  // C++98 [class.friend]p1: A friend of a class is a function
  //   or class that is not a member of the class . . .
  // This is fixed in DR77, which just barely didn't make the C++03
  // deadline.  It's also a very silly restriction that seriously
  // affects inner classes and which nobody else seems to implement;
  // thus we never diagnose it, not even in -pedantic.

  Decl *D;
  if (TempParams.size())
    D = FriendTemplateDecl::Create(Context, CurContext, Loc,
                                   TempParams.size(),
                                 (TemplateParameterList**) TempParams.release(),
                                   T.getTypePtr(),
                                   DS.getFriendSpecLoc());
  else
    D = FriendDecl::Create(Context, CurContext, Loc, T.getTypePtr(),
                           DS.getFriendSpecLoc());
  D->setAccess(AS_public);
  CurContext->addDecl(D);

  return DeclPtrTy::make(D);
}

Sema::DeclPtrTy
Sema::ActOnFriendFunctionDecl(Scope *S,
                              Declarator &D,
                              bool IsDefinition,
                              MultiTemplateParamsArg TemplateParams) {
  const DeclSpec &DS = D.getDeclSpec();

  assert(DS.isFriendSpecified());
  assert(DS.getStorageClassSpec() == DeclSpec::SCS_unspecified);

  SourceLocation Loc = D.getIdentifierLoc();
  TypeSourceInfo *TInfo = 0;
  QualType T = GetTypeForDeclarator(D, S, &TInfo);

  // C++ [class.friend]p1
  //   A friend of a class is a function or class....
  // Note that this sees through typedefs, which is intended.
  // It *doesn't* see through dependent types, which is correct
  // according to [temp.arg.type]p3:
  //   If a declaration acquires a function type through a
  //   type dependent on a template-parameter and this causes
  //   a declaration that does not use the syntactic form of a
  //   function declarator to have a function type, the program
  //   is ill-formed.
  if (!T->isFunctionType()) {
    Diag(Loc, diag::err_unexpected_friend);

    // It might be worthwhile to try to recover by creating an
    // appropriate declaration.
    return DeclPtrTy();
  }

  // C++ [namespace.memdef]p3
  //  - If a friend declaration in a non-local class first declares a
  //    class or function, the friend class or function is a member
  //    of the innermost enclosing namespace.
  //  - The name of the friend is not found by simple name lookup
  //    until a matching declaration is provided in that namespace
  //    scope (either before or after the class declaration granting
  //    friendship).
  //  - If a friend function is called, its name may be found by the
  //    name lookup that considers functions from namespaces and
  //    classes associated with the types of the function arguments.
  //  - When looking for a prior declaration of a class or a function
  //    declared as a friend, scopes outside the innermost enclosing
  //    namespace scope are not considered.

  CXXScopeSpec &ScopeQual = D.getCXXScopeSpec();
  DeclarationName Name = GetNameForDeclarator(D);
  assert(Name);

  // The context we found the declaration in, or in which we should
  // create the declaration.
  DeclContext *DC;

  // FIXME: handle local classes

  // Recover from invalid scope qualifiers as if they just weren't there.
  LookupResult Previous(*this, Name, D.getIdentifierLoc(), LookupOrdinaryName,
                        ForRedeclaration);
  if (!ScopeQual.isInvalid() && ScopeQual.isSet()) {
    // FIXME: RequireCompleteDeclContext
    DC = computeDeclContext(ScopeQual);

    // FIXME: handle dependent contexts
    if (!DC) return DeclPtrTy();

    LookupQualifiedName(Previous, DC);

    // If searching in that context implicitly found a declaration in
    // a different context, treat it like it wasn't found at all.
    // TODO: better diagnostics for this case.  Suggesting the right
    // qualified scope would be nice...
    // FIXME: getRepresentativeDecl() is not right here at all
    if (Previous.empty() ||
        !Previous.getRepresentativeDecl()->getDeclContext()->Equals(DC)) {
      D.setInvalidType();
      Diag(Loc, diag::err_qualified_friend_not_found) << Name << T;
      return DeclPtrTy();
    }

    // C++ [class.friend]p1: A friend of a class is a function or
    //   class that is not a member of the class . . .
    if (DC->Equals(CurContext))
      Diag(DS.getFriendSpecLoc(), diag::err_friend_is_member);

  // Otherwise walk out to the nearest namespace scope looking for matches.
  } else {
    // TODO: handle local class contexts.

    DC = CurContext;
    while (true) {
      // Skip class contexts.  If someone can cite chapter and verse
      // for this behavior, that would be nice --- it's what GCC and
      // EDG do, and it seems like a reasonable intent, but the spec
      // really only says that checks for unqualified existing
      // declarations should stop at the nearest enclosing namespace,
      // not that they should only consider the nearest enclosing
      // namespace.
      while (DC->isRecord()) 
        DC = DC->getParent();

      LookupQualifiedName(Previous, DC);

      // TODO: decide what we think about using declarations.
      if (!Previous.empty())
        break;
      
      if (DC->isFileContext()) break;
      DC = DC->getParent();
    }

    // C++ [class.friend]p1: A friend of a class is a function or
    //   class that is not a member of the class . . .
    // C++0x changes this for both friend types and functions.
    // Most C++ 98 compilers do seem to give an error here, so
    // we do, too.
    if (!Previous.empty() && DC->Equals(CurContext)
        && !getLangOptions().CPlusPlus0x)
      Diag(DS.getFriendSpecLoc(), diag::err_friend_is_member);
  }

  if (DC->isFileContext()) {
    // This implies that it has to be an operator or function.
    if (D.getName().getKind() == UnqualifiedId::IK_ConstructorName ||
        D.getName().getKind() == UnqualifiedId::IK_DestructorName ||
        D.getName().getKind() == UnqualifiedId::IK_ConversionFunctionId) {
      Diag(Loc, diag::err_introducing_special_friend) <<
        (D.getName().getKind() == UnqualifiedId::IK_ConstructorName ? 0 :
         D.getName().getKind() == UnqualifiedId::IK_DestructorName ? 1 : 2);
      return DeclPtrTy();
    }
  }

  bool Redeclaration = false;
  NamedDecl *ND = ActOnFunctionDeclarator(S, D, DC, T, TInfo, Previous,
                                          move(TemplateParams),
                                          IsDefinition,
                                          Redeclaration);
  if (!ND) return DeclPtrTy();

  assert(ND->getDeclContext() == DC);
  assert(ND->getLexicalDeclContext() == CurContext);

  // Add the function declaration to the appropriate lookup tables,
  // adjusting the redeclarations list as necessary.  We don't
  // want to do this yet if the friending class is dependent.
  //
  // Also update the scope-based lookup if the target context's
  // lookup context is in lexical scope.
  if (!CurContext->isDependentContext()) {
    DC = DC->getLookupContext();
    DC->makeDeclVisibleInContext(ND, /* Recoverable=*/ false);
    if (Scope *EnclosingScope = getScopeForDeclContext(S, DC))
      PushOnScopeChains(ND, EnclosingScope, /*AddToContext=*/ false);
  }

  FriendDecl *FrD = FriendDecl::Create(Context, CurContext,
                                       D.getIdentifierLoc(), ND,
                                       DS.getFriendSpecLoc());
  FrD->setAccess(AS_public);
  CurContext->addDecl(FrD);

  if (D.getName().getKind() == UnqualifiedId::IK_TemplateId)
    FrD->setSpecialization(true);

  return DeclPtrTy::make(ND);
}

void Sema::SetDeclDeleted(DeclPtrTy dcl, SourceLocation DelLoc) {
  AdjustDeclIfTemplate(dcl);

  Decl *Dcl = dcl.getAs<Decl>();
  FunctionDecl *Fn = dyn_cast<FunctionDecl>(Dcl);
  if (!Fn) {
    Diag(DelLoc, diag::err_deleted_non_function);
    return;
  }
  if (const FunctionDecl *Prev = Fn->getPreviousDeclaration()) {
    Diag(DelLoc, diag::err_deleted_decl_not_first);
    Diag(Prev->getLocation(), diag::note_previous_declaration);
    // If the declaration wasn't the first, we delete the function anyway for
    // recovery.
  }
  Fn->setDeleted();
}

static void SearchForReturnInStmt(Sema &Self, Stmt *S) {
  for (Stmt::child_iterator CI = S->child_begin(), E = S->child_end(); CI != E;
       ++CI) {
    Stmt *SubStmt = *CI;
    if (!SubStmt)
      continue;
    if (isa<ReturnStmt>(SubStmt))
      Self.Diag(SubStmt->getSourceRange().getBegin(),
           diag::err_return_in_constructor_handler);
    if (!isa<Expr>(SubStmt))
      SearchForReturnInStmt(Self, SubStmt);
  }
}

void Sema::DiagnoseReturnInConstructorExceptionHandler(CXXTryStmt *TryBlock) {
  for (unsigned I = 0, E = TryBlock->getNumHandlers(); I != E; ++I) {
    CXXCatchStmt *Handler = TryBlock->getHandler(I);
    SearchForReturnInStmt(*this, Handler);
  }
}

bool Sema::CheckOverridingFunctionReturnType(const CXXMethodDecl *New,
                                             const CXXMethodDecl *Old) {
  QualType NewTy = New->getType()->getAs<FunctionType>()->getResultType();
  QualType OldTy = Old->getType()->getAs<FunctionType>()->getResultType();

  QualType CNewTy = Context.getCanonicalType(NewTy);
  QualType COldTy = Context.getCanonicalType(OldTy);

  if (CNewTy == COldTy &&
      CNewTy.getLocalCVRQualifiers() == COldTy.getLocalCVRQualifiers())
    return false;

  // Check if the return types are covariant
  QualType NewClassTy, OldClassTy;

  /// Both types must be pointers or references to classes.
  if (PointerType *NewPT = dyn_cast<PointerType>(NewTy)) {
    if (PointerType *OldPT = dyn_cast<PointerType>(OldTy)) {
      NewClassTy = NewPT->getPointeeType();
      OldClassTy = OldPT->getPointeeType();
    }
  } else if (ReferenceType *NewRT = dyn_cast<ReferenceType>(NewTy)) {
    if (ReferenceType *OldRT = dyn_cast<ReferenceType>(OldTy)) {
      NewClassTy = NewRT->getPointeeType();
      OldClassTy = OldRT->getPointeeType();
    }
  }

  // The return types aren't either both pointers or references to a class type.
  if (NewClassTy.isNull()) {
    Diag(New->getLocation(),
         diag::err_different_return_type_for_overriding_virtual_function)
      << New->getDeclName() << NewTy << OldTy;
    Diag(Old->getLocation(), diag::note_overridden_virtual_function);

    return true;
  }

  if (!Context.hasSameUnqualifiedType(NewClassTy, OldClassTy)) {
    // Check if the new class derives from the old class.
    if (!IsDerivedFrom(NewClassTy, OldClassTy)) {
      Diag(New->getLocation(),
           diag::err_covariant_return_not_derived)
      << New->getDeclName() << NewTy << OldTy;
      Diag(Old->getLocation(), diag::note_overridden_virtual_function);
      return true;
    }

    // Check if we the conversion from derived to base is valid.
    if (CheckDerivedToBaseConversion(NewClassTy, OldClassTy,
                      diag::err_covariant_return_inaccessible_base,
                      diag::err_covariant_return_ambiguous_derived_to_base_conv,
                      // FIXME: Should this point to the return type?
                      New->getLocation(), SourceRange(), New->getDeclName())) {
      Diag(Old->getLocation(), diag::note_overridden_virtual_function);
      return true;
    }
  }

  // The qualifiers of the return types must be the same.
  if (CNewTy.getLocalCVRQualifiers() != COldTy.getLocalCVRQualifiers()) {
    Diag(New->getLocation(),
         diag::err_covariant_return_type_different_qualifications)
    << New->getDeclName() << NewTy << OldTy;
    Diag(Old->getLocation(), diag::note_overridden_virtual_function);
    return true;
  };


  // The new class type must have the same or less qualifiers as the old type.
  if (NewClassTy.isMoreQualifiedThan(OldClassTy)) {
    Diag(New->getLocation(),
         diag::err_covariant_return_type_class_type_more_qualified)
    << New->getDeclName() << NewTy << OldTy;
    Diag(Old->getLocation(), diag::note_overridden_virtual_function);
    return true;
  };

  return false;
}

bool Sema::CheckOverridingFunctionAttributes(const CXXMethodDecl *New,
                                             const CXXMethodDecl *Old)
{
  if (Old->hasAttr<FinalAttr>()) {
    Diag(New->getLocation(), diag::err_final_function_overridden)
      << New->getDeclName();
    Diag(Old->getLocation(), diag::note_overridden_virtual_function);
    return true;
  }

  return false;
}

/// \brief Mark the given method pure.
///
/// \param Method the method to be marked pure.
///
/// \param InitRange the source range that covers the "0" initializer.
bool Sema::CheckPureMethod(CXXMethodDecl *Method, SourceRange InitRange) {
  if (Method->isVirtual() || Method->getParent()->isDependentContext()) {
    Method->setPure();
    
    // A class is abstract if at least one function is pure virtual.
    Method->getParent()->setAbstract(true);
    return false;
  } 

  if (!Method->isInvalidDecl())
    Diag(Method->getLocation(), diag::err_non_virtual_pure)
      << Method->getDeclName() << InitRange;
  return true;
}

/// ActOnCXXEnterDeclInitializer - Invoked when we are about to parse
/// an initializer for the out-of-line declaration 'Dcl'.  The scope
/// is a fresh scope pushed for just this purpose.
///
/// After this method is called, according to [C++ 3.4.1p13], if 'Dcl' is a
/// static data member of class X, names should be looked up in the scope of
/// class X.
void Sema::ActOnCXXEnterDeclInitializer(Scope *S, DeclPtrTy Dcl) {
  // If there is no declaration, there was an error parsing it.
  Decl *D = Dcl.getAs<Decl>();
  if (D == 0) return;

  // We should only get called for declarations with scope specifiers, like:
  //   int foo::bar;
  assert(D->isOutOfLine());
  EnterDeclaratorContext(S, D->getDeclContext());
}

/// ActOnCXXExitDeclInitializer - Invoked after we are finished parsing an
/// initializer for the out-of-line declaration 'Dcl'.
void Sema::ActOnCXXExitDeclInitializer(Scope *S, DeclPtrTy Dcl) {
  // If there is no declaration, there was an error parsing it.
  Decl *D = Dcl.getAs<Decl>();
  if (D == 0) return;

  assert(D->isOutOfLine());
  ExitDeclaratorContext(S);
}

/// ActOnCXXConditionDeclarationExpr - Parsed a condition declaration of a
/// C++ if/switch/while/for statement.
/// e.g: "if (int x = f()) {...}"
Action::DeclResult
Sema::ActOnCXXConditionDeclaration(Scope *S, Declarator &D) {
  // C++ 6.4p2:
  // The declarator shall not specify a function or an array.
  // The type-specifier-seq shall not contain typedef and shall not declare a
  // new class or enumeration.
  assert(D.getDeclSpec().getStorageClassSpec() != DeclSpec::SCS_typedef &&
         "Parser allowed 'typedef' as storage class of condition decl.");
  
  TypeSourceInfo *TInfo = 0;
  TagDecl *OwnedTag = 0;
  QualType Ty = GetTypeForDeclarator(D, S, &TInfo, &OwnedTag);
  
  if (Ty->isFunctionType()) { // The declarator shall not specify a function...
                              // We exit without creating a CXXConditionDeclExpr because a FunctionDecl
                              // would be created and CXXConditionDeclExpr wants a VarDecl.
    Diag(D.getIdentifierLoc(), diag::err_invalid_use_of_function_type)
      << D.getSourceRange();
    return DeclResult();
  } else if (OwnedTag && OwnedTag->isDefinition()) {
    // The type-specifier-seq shall not declare a new class or enumeration.
    Diag(OwnedTag->getLocation(), diag::err_type_defined_in_condition);
  }
  
  DeclPtrTy Dcl = ActOnDeclarator(S, D);
  if (!Dcl)
    return DeclResult();

  VarDecl *VD = cast<VarDecl>(Dcl.getAs<Decl>());
  VD->setDeclaredInCondition(true);
  return Dcl;
}

void Sema::MaybeMarkVirtualMembersReferenced(SourceLocation Loc,
                                             CXXMethodDecl *MD) {
  // Ignore dependent types.
  if (MD->isDependentContext())
    return;
  
  CXXRecordDecl *RD = MD->getParent();
  
  // Ignore classes without a vtable.
  if (!RD->isDynamicClass())
    return;

  if (!MD->isOutOfLine()) {
    // The only inline functions we care about are constructors. We also defer
    // marking the virtual members as referenced until we've reached the end
    // of the translation unit. We do this because we need to know the key
    // function of the class in order to determine the key function.
    if (isa<CXXConstructorDecl>(MD))
      ClassesWithUnmarkedVirtualMembers.insert(std::make_pair(RD, Loc));
    return;
  }
  
  const CXXMethodDecl *KeyFunction = Context.getKeyFunction(RD);

  if (!KeyFunction) {
    // This record does not have a key function, so we assume that the vtable
    // will be emitted when it's used by the constructor.
    if (!isa<CXXConstructorDecl>(MD))
      return;
  } else if (KeyFunction->getCanonicalDecl() != MD->getCanonicalDecl()) {
    // We don't have the right key function.
    return;
  }
  
  // Mark the members as referenced.
  MarkVirtualMembersReferenced(Loc, RD);
  ClassesWithUnmarkedVirtualMembers.erase(RD);
}

bool Sema::ProcessPendingClassesWithUnmarkedVirtualMembers() {
  if (ClassesWithUnmarkedVirtualMembers.empty())
    return false;
  
  for (std::map<CXXRecordDecl *, SourceLocation>::iterator i = 
       ClassesWithUnmarkedVirtualMembers.begin(), 
       e = ClassesWithUnmarkedVirtualMembers.end(); i != e; ++i) {
    CXXRecordDecl *RD = i->first;
    
    const CXXMethodDecl *KeyFunction = Context.getKeyFunction(RD);
    if (KeyFunction) {
      // We know that the class has a key function. If the key function was
      // declared in this translation unit, then it the class decl would not 
      // have been in the ClassesWithUnmarkedVirtualMembers map.
      continue;
    }
    
    SourceLocation Loc = i->second;
    MarkVirtualMembersReferenced(Loc, RD);
  }
  
  ClassesWithUnmarkedVirtualMembers.clear();
  return true;
}

void Sema::MarkVirtualMembersReferenced(SourceLocation Loc, CXXRecordDecl *RD) {
  for (CXXRecordDecl::method_iterator i = RD->method_begin(), 
       e = RD->method_end(); i != e; ++i) {
    CXXMethodDecl *MD = *i;

    // C++ [basic.def.odr]p2:
    //   [...] A virtual member function is used if it is not pure. [...]
    if (MD->isVirtual() && !MD->isPure())
      MarkDeclarationReferenced(Loc, MD);
  }
}
