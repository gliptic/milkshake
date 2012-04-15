#ifndef PARSER_HPP
#define PARSER_HPP

//#pragma warning(disable : 4005)

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/LLVMContext.h"

#include <string>
#include <memory>
#include <set>
#include <map>
#include <exception>
#include <stdexcept>
#include <vector>
#include <cassert>
#include <bitset>
#include <istream>
#include <sstream>

#define OVERRIDE

using std::unique_ptr;
using std::string;
using std::map;
using std::vector;
using llvm::IntrusiveRefCntPtr;

namespace llvm
{
	class Value;
}

struct Parser;
struct Expr;
struct FuncBuilder;
struct ModuleBuilder;
struct MatchExpr;
struct Context;
struct TypeData;
struct TypeDataDef;

enum CE
{
	CENoError,
	CEUnresolvedIdentifier,
	CECyclicRef,
	CENotAssignable,
	CEParseError,
	CEUnexpectedCharacter,
	CEGeneralError,
	CEWrongParamCount,
	CETypeError,
	CEOperatorNotValidForTypes,
	CEConditionMustBeBool,
	CEUnexpectedToken,
	CEIncompleteVarDecl,
	CEUnreachablePattern
};

struct FileObject : llvm::RefCountedBaseVPTR
{
	int line;
};

extern char const* const ceNames[];

struct CompileError : std::exception
{
	CompileError(CE ceInit, int lineInit)
	: ce(ceInit), line(lineInit)
	{
		setMsg();
	}

	CompileError(CE ceInit, FileObject* obj)
	: ce(ceInit), line(obj ? obj->line : -1)
	{
		setMsg();
	}

	virtual ~CompileError() throw()
	{
	}

	void setMsg()
	{
		std::stringstream ss;
		ss << ceNames[ce] << ", at " << line;
		msg = ss.str();
	}

	char const* what() const throw()
	{
		return msg.c_str();
	}

	CE ce;
	int line;
	std::string msg;
};

struct FuncBuilder
{
	FuncBuilder(ModuleBuilder& modBuilderInit, llvm::BasicBlock* entryBb, llvm::Function* funcInit)
	: modBuilder(modBuilderInit)
	, ir(entryBb)
	, func(funcInit)
	{
	}

	ModuleBuilder& modBuilder;
	llvm::IRBuilder<> ir;
	llvm::Function* func;
};


enum Token
{
	TLBrace,
	TRBrace,
	TLParen,
	TRParen,
	TSemicolon,
	TIdent,
	TTypeIdent,
	TConstNum,
	TLet,
	TIf,
	TElse,
	TPrint,
	TAssert,
	TPlus,
	TMinus,
	TDot,
	TEqual,
	TDblEqual,
	TComma,
	TLessThan,
	TNull,
	TReturn,
	TWhile,
	TModule,
	THash,
	TCase,
	TImport,
	TClass,
	TEof,

	TCount
};

union TokenData
{
	std::string* str;
};

enum TypeKind
{
	TyNone,
	TyUnit,
	TyInt32,
	TyBool,
	TyDouble,

	TyNamed,
	TyVar,
	TyDef,
	TyAlias
};

struct TypeData : llvm::RefCountedBaseVPTR
{
	TypeData(TypeKind kindInit)
	: kind(kindInit)
	{
	}

	virtual IntrusiveRefCntPtr<TypeData> apply(vector<IntrusiveRefCntPtr<TypeData>> const& params)
	{
		assert(!params.empty());
		throw CompileError(CEGeneralError, -1);
	}

	virtual llvm::Type* llvmType(ModuleBuilder& modBuilder);

	virtual IntrusiveRefCntPtr<TypeData> doResolve(ModuleBuilder& modBuilder) = 0;

	virtual IntrusiveRefCntPtr<TypeData> fresh();

	virtual llvm::Value* coerceFrom(FuncBuilder& builder, TypeData& other, llvm::Value* value)
	{
		if(isSame(other))
			return value;

		// TODO: This shouldn't really happen.
		// Type checking should reject type errors.
		throw CompileError(CETypeError, -1);
	}

	virtual bool isAssignableFrom(TypeData const& other) const
	{
		if(kind == TyUnit)
			return true; // All types assignable to Unit
		if(this != &other)
			return false;
		return true;
	}

	bool isSame(TypeData const& other) const
	{
		if(this != &other)
			return false;
		return true;
	}

	IntrusiveRefCntPtr<TypeData> commonWith(IntrusiveRefCntPtr<TypeData> const& other);

	TypeKind kind;
};

typedef llvm::IntrusiveRefCntPtr<TypeData> TypePtr;

struct TypeDataBasic : TypeData
{
	TypeDataBasic(TypeKind kindInit)
	: TypeData(kindInit)
	{
	}

	TypePtr doResolve(ModuleBuilder&) OVERRIDE
	{
		return TypePtr(this);
	}

	llvm::Type* llvmType(ModuleBuilder& modBuilder) OVERRIDE;
};

struct TypeDataVar : TypeData
{
	TypeDataVar(int indexInit = 0)
	: TypeData(TyVar)
	, index(indexInit)
	{
	}

	TypePtr doResolve(ModuleBuilder&) OVERRIDE
	{
		return TypePtr(this);
	}

	llvm::Type* llvmType(ModuleBuilder& modBuilder) OVERRIDE;

	llvm::Value* coerceFrom(FuncBuilder& builder, TypeData& other, llvm::Value* value) OVERRIDE;

	int index;
};

typedef IntrusiveRefCntPtr<TypeDataVar> TypeVarPtr;

extern TypePtr tyUnit, tyInt32, tyDouble, tyBool;

struct TypeDataComposite : TypeData
{
	TypeDataComposite(TypeKind kindInit)
	: TypeData(kindInit)
	, modBuilder(0)
	{
	}

	void doResolve()
	{
		for(auto& t : typeAssignment)
		{
			t = t->doResolve(*modBuilder);
		}
	}

	vector<TypePtr> typeAssignment;
	ModuleBuilder* modBuilder;
};

struct TypeDataNamed : TypeDataComposite
{
	TypeDataNamed(string const& nameInit)
	: TypeDataComposite(TyNamed)
	, name(nameInit)
	{
	}

	TypePtr doResolve(ModuleBuilder& modBuilder) OVERRIDE;

	string name;
};

typedef IntrusiveRefCntPtr<Expr> ExprPtr;

enum ExprKind
{
	ExOther,
	ExFuncRef
};

struct Expr : FileObject
{
	Expr()
	: kind(ExOther)
	, resolveStarted(false)
	{
	}

	virtual llvm::Value* build(FuncBuilder& builder) = 0;
	virtual ExprPtr resolve(ModuleBuilder& modBuilder) = 0;

	ExprPtr doResolve(ModuleBuilder& modBuilder)
	{
		if(type)
			return ExprPtr(this);
		if(resolveStarted)
			throw CompileError(CECyclicRef, this);
		resolveStarted = true;
		ExprPtr const& ret = resolve(modBuilder);
		assert(ret->type && "resolve() must resolve type");
		return ret;
	}

	ExprKind kind;
	TypePtr type;
	bool resolveStarted;
};

struct VarDecl : llvm::RefCountedBaseVPTR // : Expr
{
	VarDecl(
		string const& nameInit,
		TypePtr const& declTypeInit)
	: name(nameInit)
	, declType(declTypeInit)
	, isParameter(false)
	, isImmutable(true)
	, llvmValue(0)
	{
	}

	void buildInit(FuncBuilder& builder, llvm::Value* value)
	{
		if(isImmutable)
		{
			assert(!llvmValue);
			llvmValue = value;
		}
		else
		{
			assert(llvmValue);
			builder.ir.CreateStore(value, llvmValue);
		}
	}
	string name;
	TypePtr declType;
	bool isParameter;
	bool isImmutable;

	llvm::Value* llvmValue;
};

struct VarRef : Expr
{
	VarRef(IntrusiveRefCntPtr<VarDecl> declInit)
	: decl(declInit)
	{
	}

	virtual llvm::Value* build(FuncBuilder& builder)
	{
		if(decl->isParameter || decl->isImmutable)
			return decl->llvmValue;

		return builder.ir.CreateLoad(decl->llvmValue);
	}

	virtual ExprPtr resolve(ModuleBuilder& modBuilder)
	{
		assert(decl->declType);

		type = decl->declType;
		return ExprPtr(this);
	}

	IntrusiveRefCntPtr<VarDecl> decl;
};

struct NamedRef : Expr
{
	NamedRef(string const& nameInit)
	: name(nameInit)
	{
	}

	virtual llvm::Value* build(FuncBuilder& builder)
	{
		assert(false);
		return 0;
	}

	virtual ExprPtr resolve(ModuleBuilder& modBuilder);

	string name;
};

struct Scope : Expr
{
	Scope()
	{
	}

	Scope(ExprPtr const& innerInit)
	: inner(innerInit)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		buildEntry(builder);
		auto ret = inner->build(builder);
		buildExit(builder);
		return ret;
	}

	void buildEntry(FuncBuilder& builder)
	{
	}

	void buildExit(FuncBuilder& builder)
	{
		// TODO: Release locals
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		if(inner)
		{
			inner = inner->doResolve(modBuilder);
			type = inner->type;
		}

		// If inner is 0, it's assumed that a base class sets type
		return ExprPtr(this);
	}

	IntrusiveRefCntPtr<VarDecl> resolveLocal(string const& name)
	{
		for(auto& slot : slots)
		{
			if(slot->name == name)
				return slot;
		}

		if(parentScope)
			return parentScope->resolveLocal(name);
		return IntrusiveRefCntPtr<VarDecl>();
	}

	IntrusiveRefCntPtr<Scope> parentScope;
	ExprPtr inner;
	vector<IntrusiveRefCntPtr<VarDecl>> slots;
};

struct TypeScope : llvm::RefCountedBaseVPTR
{
	TypePtr resolveIdent(string const& name)
	{
		auto i = namedTypes.find(name);
		if(i != namedTypes.end())
			return i->second;

		if(parentScope)
			return parentScope->resolveIdent(name);
		return TypePtr();
	}

	IntrusiveRefCntPtr<TypeScope> parentScope;
	map<string, TypePtr> namedTypes;
};

struct BinExpr : Expr
{
	BinExpr(ExprPtr const& a, ExprPtr const& b, Token op)
	: a(a), b(b), op(op)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		auto av = a->build(builder);
		auto bv = b->build(builder);

		switch(op)
		{
			case TPlus:
				if(a->type->kind == TyDouble)
					return builder.ir.CreateFAdd(av, bv);
				else
					return builder.ir.CreateAdd(av, bv);
			case TMinus:
				if(a->type->kind == TyDouble)
					return builder.ir.CreateFSub(av, bv);
				else
					return builder.ir.CreateSub(av, bv);
			case TDblEqual:
				if(a->type->kind == TyDouble)
					return builder.ir.CreateFCmpOEQ(av, bv);
				else
					return builder.ir.CreateICmpEQ(av, bv);
			case TLessThan:
				if(a->type->kind == TyDouble)
					return builder.ir.CreateFCmpOLT(av, bv);
				else
					return builder.ir.CreateICmpSLT(av, bv);
			default:
				throw CompileError(CEGeneralError, this);
		}
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		a = a->resolve(modBuilder);
		b = b->resolve(modBuilder);

		if(op == TPlus || op == TMinus)
		{
			if(! a->type->isAssignableFrom(*b->type))
			 	throw CompileError(CETypeError, this);
			if(a->type->kind != TyInt32
			&& a->type->kind != TyDouble)
				throw CompileError(CETypeError, this);
			type = a->type;
		}
		else if(op == TDblEqual || op == TLessThan)
		{
			if(! a->type->isAssignableFrom(*b->type))
			 	throw CompileError(CETypeError, this);
			if(a->type->kind != TyInt32
			&& a->type->kind != TyDouble)
				throw CompileError(CETypeError, this);
			type = tyBool;
		}

		return ExprPtr(this);
	}

	ExprPtr a, b;
	Token op;
};

struct IfExpr : Scope
{
	ExprPtr cond, trueCase;

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(builder.ir.getContext(), "then", builder.func);
		llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(builder.ir.getContext(), "ifcont", builder.func);

		llvm::Value* cvalue = cond->build(builder);

		builder.ir.CreateCondBr(cvalue, thenBB, mergeBB);

		builder.ir.SetInsertPoint(thenBB);
		buildEntry(builder);
		trueCase->build(builder);
		buildExit(builder);
		builder.ir.CreateBr(mergeBB);

		builder.ir.SetInsertPoint(mergeBB);

		return 0;
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		Scope::resolve(modBuilder);

		cond = cond->doResolve(modBuilder);

		if(cond->type->kind != TyBool)
			throw CompileError(CEConditionMustBeBool, cond.getPtr());
		trueCase = trueCase->doResolve(modBuilder);

		type = tyUnit;

		return ExprPtr(this);
	}
};

struct IfElseExpr : Expr
{
	IfElseExpr(
		IntrusiveRefCntPtr<IfExpr> ifInit,
		ExprPtr falseCaseInit)
	: if_(ifInit)
	, falseCase(falseCaseInit)
	{
	}

	IntrusiveRefCntPtr<IfExpr> if_;
	ExprPtr falseCase;

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(builder.ir.getContext(), "then", builder.func);
		llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(builder.ir.getContext(), "else", builder.func);
		llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(builder.ir.getContext(), "ifcont", builder.func);

		llvm::Value* cvalue = if_->cond->build(builder);
		builder.ir.CreateCondBr(cvalue, thenBB, elseBB);

		builder.ir.SetInsertPoint(thenBB);
		if_->buildEntry(builder);
		llvm::Value* trueVal = if_->trueCase->build(builder);
		thenBB = builder.ir.GetInsertBlock();
		if_->buildExit(builder);
		builder.ir.CreateBr(mergeBB);

		builder.ir.SetInsertPoint(elseBB);
		llvm::Value* falseVal = falseCase->build(builder);
		elseBB = builder.ir.GetInsertBlock();
		builder.ir.CreateBr(mergeBB);

		builder.ir.SetInsertPoint(mergeBB);
		// TODO: Coerce values if necessary
		llvm::PHINode* phi = builder.ir.CreatePHI(trueVal->getType(), 2);

		phi->addIncoming(trueVal, thenBB);
		phi->addIncoming(falseVal, elseBB);
		return phi;
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		if_ = static_cast<IfExpr*>(if_->doResolve(modBuilder).getPtr());
		falseCase = falseCase->doResolve(modBuilder);

		if_->type = if_->trueCase->type; // Fix-up return type of If. TODO: Can changing the type cause trouble?
		type = if_->trueCase->type->commonWith(falseCase->type);

		return ExprPtr(this);
	}
};


struct SeqExpr : Expr
{
	vector<ExprPtr> expressions;

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		llvm::Value* ret = 0;

		for(auto e : expressions)
		{
			ret = e->build(builder);
		}

		return ret;
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		for(ExprPtr& e : expressions)
		{
			e = e->doResolve(modBuilder);

			// TODO: Reject discarding non-unit values
		}

		if (expressions.size() > 0)
			type = expressions.back()->type;
		else
			type = tyUnit;

		return ExprPtr(this);
	}
};

struct ReturnExpr : Expr
{
	ReturnExpr(ExprPtr const& value)
	: value(value)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		llvm::Value* v = value->build(builder);
		// TODO: Exit all scopes
		builder.ir.CreateRet(v);
		return 0; // Unit
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		value = value->doResolve(modBuilder);

		type = tyUnit;
		return ExprPtr(this);
	}

	ExprPtr value;
};

struct FuncDef : llvm::RefCountedBaseVPTR
{
	FuncDef(string const& nameInit)
	: name(nameInit)
	, resolved(false)
	, resolveStarted(false)
	, compiledFunc(0)
	, modBuilder(0)
	{
	}

	void doResolve()
	{
		if(resolved)
			return;
		if(resolveStarted)
			throw CompileError(CECyclicRef, body.getPtr());
		resolveStarted = true;
		
		// TODO: Can resolving the parameters or return value
		// cause resolving of expressions?

		for(auto& p : parameters)
			p->declType = p->declType->doResolve(*modBuilder);

		if(returnType)
			returnType = returnType->doResolve(*modBuilder);

		resolved = true; // Recursive calling is fine by this point
		if(body)
		{
			body = body->doResolve(*modBuilder);

			if(!returnType)
				returnType = body->type;

			if(! returnType->isAssignableFrom(*body->type))
				throw CompileError(CETypeError, body.getPtr());
		}
		else if(!returnType)
		{
			throw CompileError(CEGeneralError, -1);
		}
	}

	void build(FuncBuilder& builder);

	string name;
	
	ExprPtr body;
	vector<IntrusiveRefCntPtr<VarDecl>> parameters;
	vector<TypeVarPtr> typeParams;
	TypePtr returnType;
	bool resolved, resolveStarted;
	vector<IntrusiveRefCntPtr<VarDecl>> locals;

	// TODO: More independent
	llvm::Function* compiledFunc;
	ModuleBuilder* modBuilder;
};

struct FuncRefExpr : Expr
{
	FuncRefExpr(IntrusiveRefCntPtr<FuncDef> defInit)
	: def(defInit)
	{
		kind = ExFuncRef;
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		// TODO
		assert(false);
		return 0;
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		// TODO: Function references do not have types at the moment
		def->doResolve();
		type = tyUnit;
		return ExprPtr(this);
	}

	IntrusiveRefCntPtr<FuncDef> def;
};

struct TypeMember
{
	TypeMember(string const& nameInit, TypePtr const& typeInit)
	: name(nameInit)
	, type(typeInit)
	{
	}

	string name;
	TypePtr type;
};

struct TypeConstructor : llvm::RefCountedBaseVPTR
{
	TypeConstructor(string const& nameInit)
	: name(nameInit)
	, ctorParams(0)
	, llvmType(0)
	{
	}

	llvm::PointerType* getLlvmType(ModuleBuilder& modBuilder);

	void doResolve(ModuleBuilder& modBuilder)
	{
		// Resolve all constructors
		type = type->doResolve(modBuilder);
	}

	TypeDataDef& typeData();

	string name;
	int tag;
	vector<TypeMember> members;
	int ctorParams;
	TypePtr type;

	/*
	int _rc;
	...
	*/
	llvm::StructType* llvmType;
	llvm::PointerType* llvmTypePtr;
};

struct TypeDataAlias : TypeDataComposite
{
	TypeDataAlias()
	: TypeDataComposite(TyAlias)
	, resolveStarted(false)
	{
	}

	TypePtr doResolve(ModuleBuilder& /*modBuilder*/) OVERRIDE;

	TypePtr aliasFor;
	bool resolveStarted;
};

struct TypeDef : llvm::RefCountedBaseVPTR
{
	TypeDef(string const& nameInit)
	: name(nameInit)
	, llvmType(0)
	, resolved(false)
	, resolveStarted(false)
	{
		
	}

	void doResolve(ModuleBuilder& modBuilder)
	{
		if(!resolved)
		{
			if(resolveStarted)
				throw CompileError(CECyclicRef, -1);
			resolveStarted = true;

			int tag = 0;
		
			resolved = true; // Allow cyclic references in constructors

			for(auto& ctor : constructors)
			{
				ctor->tag = tag++;

				for(auto& member : ctor->members)
				{
					member.type = member.type->doResolve(modBuilder);
				}
			}
		}
	}

	string name;

	vector<IntrusiveRefCntPtr<TypeConstructor>> constructors;
	vector<TypeVarPtr> typeParams;
	llvm::PointerType* llvmType;
	bool resolved, resolveStarted;

	// TODO: Make more independent of the specific builder
	
};

struct TypeDataDef : TypeDataComposite
{
	TypeDataDef(IntrusiveRefCntPtr<TypeDef> const& defInit)
	: TypeDataComposite(TyDef)
	, def(defInit)
	{
	}

	llvm::PointerType* llvmType(ModuleBuilder& /*modBuilder*/) OVERRIDE;

	TypePtr doResolve(ModuleBuilder& modBuilder) OVERRIDE;

	IntrusiveRefCntPtr<TypeData> apply(vector<TypePtr> const& params) OVERRIDE;

	llvm::Value* coerceFrom(FuncBuilder& builder, TypeData& other, llvm::Value* value) OVERRIDE;

	IntrusiveRefCntPtr<TypeDef> def;
};

struct CallExpr : Expr
{
	ExprPtr func;
	vector<ExprPtr> parameters;

	llvm::Value* build(FuncBuilder& builder) OVERRIDE;

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		func = func->doResolve(modBuilder);
		for(auto& p : parameters)
			p = p->doResolve(modBuilder);

		// Currently we require a direct function reference
		//if (Func is FuncRefExpr)
		if(func->kind == ExFuncRef)
		{
			auto funcRef = static_cast<FuncRefExpr*>(func.getPtr());

			// Check parameter count
			if (funcRef->def->parameters.size() != parameters.size())
				throw CompileError(CEWrongParamCount, this);

			vector<TypePtr> typeAssignment(funcRef->def->typeParams.size());

			if(!typeAssignment.empty())
			{
				funcRef = funcRef;
			}

			// Check types
			for(auto i = 0u; i < parameters.size(); ++i)
			{
				auto destType = funcRef->def->parameters[i]->declType;

				if(destType->kind == TyVar)
				{
					TypePtr& assignment = typeAssignment[static_cast<TypeDataVar&>(*destType).index];

					if(!assignment)
						assignment = parameters[i]->type;

					destType = assignment;
				}
					
				if(!destType->isAssignableFrom(*parameters[i]->type))
					throw CompileError(CETypeError, this);
			}

			// Check that all type parameters are assigned
			for(auto& a : typeAssignment)
			{
				if(!a)
					throw CompileError(CEGeneralError, this);
			}
			
			type = funcRef->def->returnType;
			if(type->kind == TyVar)
				type = typeAssignment[static_cast<TypeDataVar&>(*type).index];
		}
		else
		{
			throw CompileError(CEGeneralError, this);
		}

		return ExprPtr(this);
	}
};


struct CreateExpr : Expr
{
	IntrusiveRefCntPtr<TypeConstructor> ctor;
	vector<ExprPtr> parameters;

	llvm::Value* build(FuncBuilder& builder) OVERRIDE;
	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE;
};

struct ConstExpr : Expr
{
	ConstExpr(int32_t valueInit)
	: value(valueInit)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		return builder.ir.getInt32(value);
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		type = tyInt32;
		return ExprPtr(this);
	}

	int32_t value;
};

struct FConstExpr : Expr
{
	FConstExpr(double valueInit)
	: value(valueInit)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE
	{
		return llvm::ConstantFP::get(type->llvmType(builder.modBuilder), value);
	}

	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE
	{
		type = tyDouble;
		return ExprPtr(this);
	}

	double value;
};

struct PatternExpr : llvm::RefCountedBaseVPTR
{
	virtual void doResolve(
		ModuleBuilder& modBuilder,
		TypePtr const& valueType) = 0;

	// matchOk and matchFail may be 0 if
	// the pattern is non-rejecting.
	// This is supposed to be checked
	// before the build stage, but isn't
	// at the moment.
	virtual bool build(
		FuncBuilder& builder,
		llvm::Value* value,
		llvm::BasicBlock* matchOk,
		llvm::BasicBlock* matchFail) = 0;
};

struct PatternBindingExpr : PatternExpr
{
	IntrusiveRefCntPtr<VarDecl> var;

	virtual void doResolve(
		ModuleBuilder& modBuilder,
		TypePtr const& valueType) OVERRIDE;

	virtual bool build(
		FuncBuilder& builder,
		llvm::Value* value,
		llvm::BasicBlock* matchOk,
		llvm::BasicBlock* matchFail) OVERRIDE;
};

struct PatternNamedCtorExpr : PatternExpr
{
	virtual void doResolve(
		ModuleBuilder& modBuilder,
		TypePtr const& valueType) OVERRIDE;

	virtual bool build(
		FuncBuilder& builder,
		llvm::Value* value,
		llvm::BasicBlock* matchOk,
		llvm::BasicBlock* matchFail) OVERRIDE;

	string name;
	IntrusiveRefCntPtr<TypeConstructor> ctor;
	vector<IntrusiveRefCntPtr<PatternExpr>> subPatterns;
};

struct MatchLeg
{
	MatchLeg(IntrusiveRefCntPtr<PatternExpr> patternInit, ExprPtr bodyInit)
	: pattern(patternInit)
	, body(bodyInit)
	{
	}

	IntrusiveRefCntPtr<PatternExpr> pattern;
	ExprPtr body;
};

struct MatchExpr : Expr
{
	MatchExpr(ExprPtr valueInit)
	: value(valueInit)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE;
	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE;

	ExprPtr value;
	vector<MatchLeg> legs;
};

struct MatchSingleNonRejectExpr : Expr
{
	MatchSingleNonRejectExpr(ExprPtr valueInit, IntrusiveRefCntPtr<PatternExpr> patternInit)
	: value(valueInit)
	, pattern(patternInit)
	{
	}

	llvm::Value* build(FuncBuilder& builder) OVERRIDE;
	ExprPtr resolve(ModuleBuilder& modBuilder) OVERRIDE;

	ExprPtr value;
	IntrusiveRefCntPtr<PatternExpr> pattern;
};

struct Import
{
	Import(string const& nameInit)
	: name(nameInit)
	{
	}

	string name;
};

struct Class : llvm::RefCountedBaseVPTR
{
	Class(string const& nameInit)
	: name(nameInit)
	{
	}

	string name;
	vector<TypeVarPtr> typeParams;
};

struct Module : llvm::RefCountedBaseVPTR
{
	string name;
	vector<string> exportedSymbols;
	vector<Import> imports;

	// TODO: Separate exported symbols
	map<string, IntrusiveRefCntPtr<FuncDef>> functions;
	map<string, TypePtr> typeDefs;
	map<string, IntrusiveRefCntPtr<TypeConstructor>> constructorDefs;
	map<string, IntrusiveRefCntPtr<Class>> classDefs;
};

struct Parser
{
	Parser(char const* beg, char const* end)
	: cur(beg), end(end)
	, currentLine(0)
	{
		firstControlSeq.set(TIf);
		firstControlSeq.set(TWhile);

		firstExpression |= firstControlSeq;

		firstExpression.set(TIdent);
		firstExpression.set(TTypeIdent);
		firstExpression.set(TConstNum);
		firstExpression.set(TLParen);
		firstExpression.set(TIf);
		firstExpression.set(TElse);
		firstExpression.set(TNull);
		firstExpression.set(TAssert);
		firstExpression.set(TPrint);
		firstExpression.set(TCase);

		firstStatement |= firstExpression;
		firstStatement.set(TLet);

		firstExpressionRest.set(TPlus);
		firstExpressionRest.set(TMinus);
		firstExpressionRest.set(TEqual);
		firstExpressionRest.set(TDblEqual);
		firstExpressionRest.set(TLessThan);

		firstSuffixOp.set(TLParen);
			
		firstType.set(TTypeIdent);

		std::memset(operatorPrecedence, 0, sizeof(operatorPrecedence));
		operatorPrecedence[TPlus] = 3;
		operatorPrecedence[TMinus] = 3;
		operatorPrecedence[TEqual] = 1;
		operatorPrecedence[TDblEqual] = 2;
		operatorPrecedence[TLessThan] = 2;

		keywords["let"] = TLet;
		keywords["if"] = TIf;
		keywords["else"] = TElse;
		keywords["print"] = TPrint;
		keywords["assert"] = TAssert;
		keywords["null"] = TNull;
		keywords["return"] = TReturn;
		keywords["while"] = TWhile;
		keywords["module"] = TModule;
		keywords["case"] = TCase;
		keywords["import"] = TImport;
		keywords["class"] = TClass;
		
		nextch();
		next();
	}

	void nextch()
	{
		if(cur != end)
		{
			ch = *cur++;
			putchar(ch);
		}
		else
		{
			ch = 0;
		}
	}

	void next(bool se = false);
	void expect(Token t, bool se = false);
	void expectPeek(Token t);
	string expectIdent(bool se = false);
	bool test(Token t, bool se = false);
	string nextIdent(bool se = false);
	
	double tokenNum(bool se = false)
	{
		string ret = tokenStr;
		next(se);
		return std::strtod(ret.c_str(), 0);
	}

	void parseError()
	{
		printf("Error on line %d, token %d\n", currentLine, token);
		throw CompileError(CEParseError, currentLine);
	}

	template<typename T>
	T* inScope(T* obj)
	{
		obj->line = currentLine;
		return obj;
	}

	void enterTypeScope()
	{
		IntrusiveRefCntPtr<TypeScope> sc(new TypeScope);
		sc->parentScope = currentTypeScope;
		currentTypeScope = sc;
	}

	void exitTypeScope()
	{
		currentTypeScope = currentTypeScope->parentScope;
	}

	template<typename T>
	IntrusiveRefCntPtr<T> enterScope(T* scope)
	{
		inScope(scope);
		scope->parentScope = currentScope;

		IntrusiveRefCntPtr<T> ret(scope);
		currentScope = ret.getPtr();
		return ret;
	}

	template<typename T>
	IntrusiveRefCntPtr<T> exitScope(ExprPtr inner)
	{
		auto old = currentScope;
		old->inner = inner;
		currentScope = old->parentScope;
		return IntrusiveRefCntPtr<T>(static_cast<T*>(old.getPtr()));
	}

	void statement(SeqExpr& seqExpr);
	ExprPtr block(bool se);
	ExprPtr statements();
	ExprPtr controlSeqExpression(bool se);
	ExprPtr primaryExpression(bool se);
	ExprPtr expressionRest(ExprPtr lhs, int minpred, bool se);
	ExprPtr expression(bool se);
	TypePtr typeName(bool se);
	TypePtr maybeType(bool se);
	TypePtr typeNameOrNewVar(bool se);
	IntrusiveRefCntPtr<VarDecl> declParam(IntrusiveRefCntPtr<FuncDef> func, string const& name, TypePtr type, int* count);
	IntrusiveRefCntPtr<VarDecl> declVar(string const& name, TypePtr type);
	IntrusiveRefCntPtr<Module> module();
	void constructor(string const& name, IntrusiveRefCntPtr<TypeDef> type, TypePtr const& typeRef);
	void structBody();
	void varDecl(SeqExpr& seqExpr, bool se);
	ExprPtr patternMatch(bool se);
	IntrusiveRefCntPtr<PatternExpr> pattern(bool se);
	void typeParamList(vector<TypeVarPtr>& list, bool se);
	IntrusiveRefCntPtr<FuncDef> funcDef(bool allowImplicitTypeVars);
	void enterFunction(IntrusiveRefCntPtr<FuncDef> func);
	void exitFunction();

	TypePtr resolveIdentType(string const& name);


	IntrusiveRefCntPtr<VarDecl> resolveLocal(
		IntrusiveRefCntPtr<Scope> sc,
		string const& name);

	ExprPtr resolveIdent(string const& name);

	void buildAst(IntrusiveRefCntPtr<Module> module);

	char const* cur;
	char const* end;

	int currentLine;
	char ch;
	Token token;
	string tokenStr;
	IntrusiveRefCntPtr<Module> mod;
	map<string, Token> keywords;
	IntrusiveRefCntPtr<Scope> currentScope;
	IntrusiveRefCntPtr<FuncDef> currentFunction;

	IntrusiveRefCntPtr<TypeScope> currentTypeScope;

	std::bitset<TCount> firstControlSeq, firstExpression, firstStatement,
		firstExpressionRest, firstSuffixOp, firstType;

	int operatorPrecedence[TCount];
};


struct ModuleBuilder : llvm::RefCountedBaseVPTR
{
	ModuleBuilder(Context& contextInit, IntrusiveRefCntPtr<Module> moduleInit);

	void buildAst();
	ExprPtr resolveIdent(string const& name);
	TypePtr resolveType(string const& name);
	void doResolve();

	IntrusiveRefCntPtr<Module> module;

	vector<IntrusiveRefCntPtr<ModuleBuilder>> imports;

	// TODO: FuncDef and TypePtr easily forms cycles. We don't
	// care that much about it since this is a bootstrap implementation.
	llvm::Module *M;

	llvm::LLVMContext& llvmContext;

	llvm::Function* abortFunc;

	// TODO: 
	Context& context;
};

struct Context
{
	Context();

	IntrusiveRefCntPtr<ModuleBuilder> addModule(IntrusiveRefCntPtr<Module> mod)
	{
		auto modBuilder = (modules[mod->name] = new ModuleBuilder(*this, mod));
		// modBuilder->doResolve();
		resolveQueue.push_back(modBuilder);
		return modBuilder;
	}

	IntrusiveRefCntPtr<ModuleBuilder> getModule(string const& name);
	IntrusiveRefCntPtr<ModuleBuilder> compileModule(std::istream& in);
	void doResolve();

	map<string, IntrusiveRefCntPtr<ModuleBuilder>> modules;
	vector<IntrusiveRefCntPtr<ModuleBuilder>> resolveQueue;
	llvm::LLVMContext llvmContext;

	llvm::StructType* typeObj;
	llvm::PointerType* typeObjPtr;
	llvm::StructType* typeTaggedObj;
	llvm::PointerType* typeTaggedObjPtr;
};

#endif // PARSER_HPP
