#include "parser.hpp"

#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/IRBuilder.h"

#include <fstream>

char const* const ceNames[] =
{
	"No error",
	"Unresolved identifier '%s'",
	"Unresolvable cyclic reference",
	"Expression is not assignable",
	"Parse error",
	"Unexpected character",
	"General error or limitation",
	"Wrong number of parameters in call",
	"Type error",
	"The operator is not valid for the given types",
	"The condition in an if- or while-expression must be of type Bool",
	"Unexpected token",
	"Variable declaration need either a type or an initializer",
	"Unreachable pattern in case-expression"
};

TypePtr tyUnit(new TypeDataBasic(TyUnit));
TypePtr tyInt32(new TypeDataBasic(TyInt32));
TypePtr tyDouble(new TypeDataBasic(TyDouble));
TypePtr tyBool(new TypeDataBasic(TyBool));

void Parser::constructor(string const& name, IntrusiveRefCntPtr<TypeDef> type, TypePtr const& typeRef)
{
	IntrusiveRefCntPtr<TypeConstructor> ctor(new TypeConstructor(name));
	type->constructors.push_back(ctor);
	ctor->type = typeRef;

	expect(TLParen);

	if(token != TRParen)
	{
		do
		{
			bool isLet = test(TLet);

			string name;

			// TODO: Common pattern?
			TypePtr type = maybeType(false);
			if(! type)
			{
				name = expectIdent();
				type = typeName(false);
			}

			ctor->members.push_back(TypeMember(name, type));
			++ctor->ctorParams;
		}
		while(test(TComma));
	}

	{
		string const& ctorName = name;
		// Constructor function
		IntrusiveRefCntPtr<FuncDef> ctorFunc(new FuncDef(ctorName));

		enterFunction(ctorFunc);
		enterScope(new Scope);

		// Constructor function has the same type parameters
		for(auto& p : type->typeParams)
			ctorFunc->typeParams.push_back(p);

		int dummy = 0;

		IntrusiveRefCntPtr<CreateExpr> body(new CreateExpr());

		body->ctor = ctor;

		for(int i = 0; i < ctor->ctorParams; ++i)
		{
			auto& member = ctor->members[i];
			auto p = declParam(ctorFunc, member.name, member.type->fresh(), &dummy);
			body->parameters.push_back(ExprPtr(new VarRef(p)));
		}

		ctorFunc->body = exitScope<Expr>(body);
		ctorFunc->returnType = typeRef;

		// TODO: Check for name collision
		mod->functions[ctorName] = ctorFunc;
		mod->constructorDefs[ctorName] = ctor;
		exitFunction();
	}

	expect(TRParen, true);
}

void Parser::typeParamList(vector<TypeVarPtr>& list, bool se)
{
	if(test(TLParen))
	{
		if(token != TRParen)
		{
			do
			{
				string const& name = expectIdent();

				IntrusiveRefCntPtr<TypeDataVar> typeVarData(
					new TypeDataVar((int)list.size()));
				TypePtr typeVar(typeVarData.getPtr());
				currentTypeScope->namedTypes[name] = typeVar;

				list.push_back(typeVarData);
			}
			while(test(TComma));
		}

		expect(TRParen, se);
	}
}

void Parser::structBody()
{
	if(token == TTypeIdent)
	{
		// <name> #( <parameters> ) { }
		string name = nextIdent(false);

		TypePtr typeRef;

		if(test(TEqual))
		{
			IntrusiveRefCntPtr<TypeDataAlias> typeDataAlias(new TypeDataAlias());

			if(token != TIdent && token != TTypeIdent)
				parseError();

			typeDataAlias->aliasFor = typeName(true);
			typeRef = typeDataAlias.getPtr();
		}
		else
		{
			IntrusiveRefCntPtr<TypeDef> type(new TypeDef(name));
			IntrusiveRefCntPtr<TypeDataDef> typeData(new TypeDataDef(type));

			typeRef = typeData.getPtr();

			enterTypeScope();

			typeParamList(type->typeParams, false);

			if(test(THash))
			{
				constructor(name, type, typeRef);
			}

			if(test(TLBrace))
			{
				do
				{
					if (token == TTypeIdent)
					{
						string const& ctorName = nextIdent(true);

						constructor(ctorName, type, typeRef);
					}
				}
				while (test(TSemicolon));

				expect(TRBrace, true);
			}

			exitTypeScope();
		}

		// TODO: Check for name collision
		mod->typeDefs[name] = typeRef;
	}
}

void Parser::enterFunction(IntrusiveRefCntPtr<FuncDef> func)
{
	currentFunction = func;
}

void Parser::exitFunction()
{
	currentFunction = 0;
}

TypePtr Parser::maybeType(bool se)
{
	if(token == TIdent)
	{
		TypePtr t = resolveIdentType(tokenStr);
		if(t)
			next(se);
		return t;
	}
	else if(token == TTypeIdent)
	{
		IntrusiveRefCntPtr<TypeDataNamed> named(
			new TypeDataNamed(nextIdent(se)));

		if(test(TLParen))
		{
			do
			{
				named->typeAssignment.push_back(typeName(false));
			}
			while(test(TComma));

			expect(TRParen);
		}

		return TypePtr(named);
	}

	parseError();
	return TypePtr();
}

TypePtr Parser::typeNameOrNewVar(bool se)
{
	TypePtr ret = maybeType(se);
	if(! ret)
	{
		if(token == TIdent)
		{
			string const& name = nextIdent(se);

			IntrusiveRefCntPtr<TypeDataVar> typeVarData(
				new TypeDataVar((int)currentFunction->typeParams.size()));
			TypePtr typeVar(typeVarData.getPtr());
			currentTypeScope->namedTypes[name] = typeVar;

			currentFunction->typeParams.push_back(typeVarData);
			ret = typeVar;
		}
		else
		{
			parseError();
		}
	}

	return ret;
}

TypePtr Parser::typeName(bool se)
{
	TypePtr ret = maybeType(se);
	if(! ret)
		parseError();
	return ret;
}

IntrusiveRefCntPtr<VarDecl> Parser::declParam(
	IntrusiveRefCntPtr<FuncDef> func,
	string const& name,
	TypePtr type,
	int* count)
{
	IntrusiveRefCntPtr<VarDecl> p(new VarDecl(name, type));
	p->isParameter = true;
	currentScope->slots.push_back(p);
	++(*count);
	func->parameters.push_back(p);
	return p;
}

IntrusiveRefCntPtr<VarDecl> Parser::declVar(string const& name, TypePtr type)
{
	IntrusiveRefCntPtr<VarDecl> varDecl(new VarDecl(name, type));
	varDecl->isParameter = false;

	currentScope->slots.push_back(varDecl);
	currentFunction->locals.push_back(varDecl);
	return varDecl;
}

IntrusiveRefCntPtr<FuncDef> Parser::funcDef(bool allowImplicitTypeVars)
{
	string const& name = nextIdent(false);

	// TODO: Always surround this in a type scope?

	IntrusiveRefCntPtr<FuncDef> def(new FuncDef(name));

	int paramCount = 0;
	enterFunction(def);
	enterScope(new Scope);

	if(test(TLParen))
	{
		if(token != TRParen)
		{
			do
			{
				TypePtr type;
				string paramName;
				if(allowImplicitTypeVars)
				{
					paramName = expectIdent();
					type = typeNameOrNewVar(false);
				}
				else
				{
					type = maybeType(false);
					if(! type)
					{
						paramName = expectIdent();
						type = typeName(false);
					}
				}

				declParam(def, paramName, type, &paramCount);
			}
			while(test(TComma));
		}

		expect(TRParen, true);
	}

	TypePtr returnType;
			
	if(firstType[token] || token == TIdent) // TODO: Types may be TIdent too, in certain circumstances
	{
		returnType = typeNameOrNewVar(true);
	}

	if(test(TLBrace))
	{
		auto const& body = statements();
		expect(TRBrace, true);

		def->body = exitScope<Expr>(body);
	}
	
	def->returnType = returnType;
	assert(paramCount == def->parameters.size());

	// TODO: Check for name collision
	mod->functions[def->name] = def;
	exitFunction();

	return def;
}

IntrusiveRefCntPtr<Module> Parser::module()
{
	mod = new Module;

	expect(TModule);

	expectPeek(TTypeIdent);
	mod->name = nextIdent();

	expect(TLParen);

	if(token != TRParen)
	{
		do
		{
			if(token != TIdent && token != TTypeIdent)
				throw CompileError(CEUnexpectedToken, -1);
			string const& name = nextIdent();
			mod->exportedSymbols.push_back(name);
		}
		while(test(TComma));
	}

	expect(TRParen, true);

	do
	{
		if(test(TImport))
		{
			expectPeek(TTypeIdent);
			string const& name = nextIdent(true);

			mod->imports.push_back(Import(name));
		}
		else if(token != TSemicolon) // TODO: Make into set
		{
			break;
		}
	}
	while(test(TSemicolon));

	do
	{
		if(token == TTypeIdent)
		{
			structBody();
		}
		else if(token == TIdent)
		{
			enterTypeScope();
			funcDef(true);
			exitTypeScope();
		}
		else if(test(TClass))
		{
			expectPeek(TTypeIdent);
			string const& name = nextIdent(false);

			IntrusiveRefCntPtr<Class> def(new Class(name));

			enterTypeScope();

			typeParamList(def->typeParams, false);

			expect(TLBrace);

			do
			{
				if (token == TIdent)
				{
					funcDef(false);
				}
			}
			while (test(TSemicolon));

			expect(TRBrace, true);

			exitTypeScope();

			mod->classDefs[def->name] = def;
		}
	}
	while(test(TSemicolon));

	expect(TEof);

	return mod;
}

void FuncDef::build(FuncBuilder& builder)
{
	assert(builder.func->arg_size() == parameters.size());
	auto arg = builder.func->arg_begin();

	for(IntrusiveRefCntPtr<VarDecl> p : parameters)
	{
		arg->setName(p->name);
		p->llvmValue = arg;
		++arg;
	}

	for(auto const& l : locals)
	{
		if(!l->isImmutable)
			l->llvmValue = builder.ir.CreateAlloca(l->declType->llvmType(builder.modBuilder));
	}
}

ExprPtr NamedRef::resolve(ModuleBuilder& modBuilder)
{
	auto resolvedExpr = modBuilder.resolveIdent(name);
	if(!resolvedExpr)
		throw CompileError(CEUnresolvedIdentifier, 0);
	return resolvedExpr->doResolve(modBuilder);
}

llvm::Value* CallExpr::build(FuncBuilder& builder)
{
	// TODO: Check expression type
	//if (Func is FuncRefExpr)
	{
			
		auto funcRef = static_cast<FuncRefExpr*>(func.getPtr());
		auto compiledFunc = funcRef->def->compiledFunc;

		std::vector<llvm::Value*> ArgsV;

		for(auto i = 0u; i < parameters.size(); ++i)
		{
			auto p = parameters[i];
			auto pv = p->build(builder);
			// Bitcast if necessary. TODO: Add coercion functions.

			pv = funcRef->def->parameters[i]->declType->coerceFrom(
				builder, *p->type, pv);
			ArgsV.push_back(pv);
		}

		llvm::Value* rv = builder.ir.CreateCall(compiledFunc, ArgsV, "calltmp");
		// auto rt = type->llvmType(builder.modBuilder);
		// Bitcast if necessary. TODO: Add coercion functions.
		rv = type->coerceFrom(builder, *funcRef->def->returnType, rv);
		return rv;
	}
}

llvm::Value* CreateExpr::build(FuncBuilder& builder)
{
	auto llvmTypePtr = ctor->getLlvmType(builder.modBuilder);
	auto llvmType = llvmTypePtr->getTypeAtIndex(0u);
	auto BB = builder.ir.GetInsertBlock();
	auto sizeOf = llvm::ConstantExpr::getSizeOf(llvmType);
	auto alloc = llvm::CallInst::CreateMalloc(
		BB,
		sizeOf->getType(),
		llvmType,
		sizeOf);

	BB->getInstList().push_back(alloc);

	for(auto i = 0u; i < parameters.size(); ++i)
	{
		auto slot = builder.ir.CreateStructGEP(alloc, i + 2);
		auto value = parameters[i]->build(builder);
		builder.ir.CreateStore(value, slot);
	}

	auto rcSlot = builder.ir.CreateStructGEP(alloc, 0);
	builder.ir.CreateStore(builder.ir.getInt32(1), rcSlot);

	auto tagSlot = builder.ir.CreateStructGEP(alloc, 1);
	builder.ir.CreateStore(builder.ir.getInt32(ctor->tag), tagSlot);

	return builder.ir.CreateBitCast(alloc, type->llvmType(builder.modBuilder));
}

ExprPtr CreateExpr::resolve(ModuleBuilder& modBuilder)
{
	for(auto& p : parameters)
		p = p->doResolve(modBuilder);

	ctor->doResolve(modBuilder);

	// ctor->type will be a non-applied type. 

	type = ctor->type;

	return ExprPtr(this);
}

llvm::Type* TypeDataBasic::llvmType(ModuleBuilder& modBuilder)
{
	switch(kind)
	{
		case TyInt32:
			return llvm::Type::getInt32Ty(modBuilder.llvmContext);

		case TyBool:
			return llvm::Type::getInt1Ty(modBuilder.llvmContext);

		case TyDouble:
			return llvm::Type::getDoubleTy(modBuilder.llvmContext);

		case TyUnit:
			return 0;

		default:
			assert(false && "kind should not have this value");
			return 0;
	}
}

llvm::Value* TypeDataVar::coerceFrom(FuncBuilder& builder, TypeData& other, llvm::Value* value)
{
	if(other.kind == TyDef)
		return builder.ir.CreateBitCast(value, llvmType(builder.modBuilder));

	return TypeData::coerceFrom(builder, other, value);
}

llvm::Type* TypeDataVar::llvmType(ModuleBuilder& modBuilder)
{
	return modBuilder.context.typeObjPtr;
}

llvm::Type* TypeData::llvmType(ModuleBuilder& modBuilder)
{
	assert(false && "No corresponding LLVM type");
	return 0;
}

TypePtr TypeData::commonWith(TypePtr const& other)
{
	if(this != other.getPtr()) // TODO: TypeData may not be interned
		return tyUnit;
			
	return TypePtr(this);
}

TypePtr TypeData::fresh()
{
/*
	if(kind == TyVar)
	{
		IntrusiveRefCntPtr<TypeData> data(
			new TypeDataVar(*static_cast<TypeDataVar*>(data.getPtr())));
		return TypeRef(TyVar, data);
	}
	else if(kind == TyNamed)
	{
		vector<TypeRef> newAssignment;
		auto& def = *static_cast<TypeDataDef*>(data.getPtr());

		bool different = false;

		for(auto& t : def.typeAssignment)
		{
			TypeRef newT = t.fresh();
			newAssignment.push_back(newT);

			different = different || !newT.isSame(t);
		}

		if(different)
		{
			
		}
	}
	
	assert(kind != TyDef);
*/
	return TypePtr(this);
}

TypePtr TypeDataNamed::doResolve(ModuleBuilder& modBuilder)
{
	if(name == "Int32_")
	{
		return tyInt32;
	}
	else if(name == "Bool_")
	{
		return tyBool;
	}
	else if(name == "Unit_")
	{
		return tyUnit;
	}
	else if(name == "Double_")
	{
		return tyDouble;
	}
	else
	{
		TypePtr newType = modBuilder.resolveType(name);

		TypeDataComposite::doResolve();

		if(!typeAssignment.empty())
			newType = newType->apply(typeAssignment);

		assert(newType->kind != TyNamed);
		return newType;
	}
}

TypePtr TypeDataDef::doResolve(ModuleBuilder& /*modBuilder*/)
{
	def->doResolve(*modBuilder);
	TypeDataComposite::doResolve();
	return TypePtr(this);
}

TypePtr TypeDataDef::apply(vector<TypePtr> const& params)
{
	assert(!params.empty());
		
	IntrusiveRefCntPtr<TypeDataDef> newData(new TypeDataDef(*this));

	newData->typeAssignment.insert(
		newData->typeAssignment.end(),
		params.begin(),
		params.end());

	// Check "over-application"
	if(newData->typeAssignment.size() > def->typeParams.size())
		throw CompileError(CEGeneralError, -1);

	return TypePtr(newData);
}

llvm::Value* TypeDataDef::coerceFrom(FuncBuilder& builder, TypeData& other, llvm::Value* value)
{
	if(other.kind == TyVar)
		return builder.ir.CreateBitCast(value, llvmType(builder.modBuilder));

	return TypeData::coerceFrom(builder, other, value);
}

TypePtr TypeDataAlias::doResolve(ModuleBuilder& /*modBuilder*/)
{
	if(resolveStarted)
		throw CompileError(CECyclicRef, -1);
	resolveStarted = true;
	aliasFor = aliasFor->doResolve(*modBuilder);
	resolveStarted = false;
	return aliasFor;
}

ModuleBuilder::ModuleBuilder(
	Context& contextInit,
	IntrusiveRefCntPtr<Module> moduleInit)
: module(moduleInit), M(0)
, llvmContext(contextInit.llvmContext)
, context(contextInit)
{
	for(auto const& func : module->functions)
	{
		func.second->modBuilder = this;
	}

	for(auto const& typeDef : module->typeDefs)
	{
		if(typeDef.second->kind == TyDef || typeDef.second->kind == TyAlias)
			static_cast<TypeDataComposite&>(*typeDef.second).modBuilder = this;
	}
}

Context::Context()
{
	vector<llvm::Type*> elements;

	// _rc
	elements.push_back(llvm::Type::getInt32Ty(llvmContext));
	typeObj = llvm::StructType::create(llvmContext, elements, "Obj");
	typeObjPtr = typeObj->getPointerTo(0);

	// _tag
	elements.push_back(llvm::Type::getInt32Ty(llvmContext));
	typeTaggedObj = llvm::StructType::create(llvmContext, elements, "TaggedObj");
	typeTaggedObjPtr = typeTaggedObj->getPointerTo(0);
}

IntrusiveRefCntPtr<ModuleBuilder> Context::getModule(string const& name)
{
	if(!modules[name])
	{
		std::ifstream f((name + ".mlk").c_str());

		return compileModule(f);
	}

	return modules[name];
}

void Context::doResolve()
{
	do
	{
		vector<IntrusiveRefCntPtr<ModuleBuilder>> localQueue;
		localQueue.swap(resolveQueue);
		
		for(auto const& mod : localQueue)
			mod->doResolve();
	}
	while(! resolveQueue.empty());
}

llvm::PointerType* TypeConstructor::getLlvmType(ModuleBuilder& modBuilder)
{
	if(! llvmType)
	{
		vector<llvm::Type*> elements;

		// _rc
		elements.push_back(llvm::Type::getInt32Ty(modBuilder.llvmContext));
		// _tag
		elements.push_back(llvm::Type::getInt32Ty(modBuilder.llvmContext));

		for(auto const& m : members)
		{
			elements.push_back(m.type->llvmType(modBuilder));
		}

		llvmType = llvm::StructType::create(modBuilder.llvmContext, elements, name);
		llvmTypePtr = llvmType->getPointerTo(0);
	}

	return llvmTypePtr;
}

TypePtr ModuleBuilder::resolveType(string const& name)
{
	auto t = module->typeDefs.find(name);
	if(t != module->typeDefs.end())
	{
		t->second = t->second->doResolve(*this);
		return t->second;
	}

	// Check imports
	for(auto const& imp : imports)
	{
		auto t = imp->module->typeDefs.find(name);
		if(t != imp->module->typeDefs.end())
		{
			t->second = t->second->doResolve(*imp);
			return t->second;
		}
	}

	throw CompileError(CETypeError, -1);
}

// All modules need to be resolved before they are built
void ModuleBuilder::doResolve()
{
	if(M)
		return; // OK

	M = new llvm::Module("test", llvmContext);

	// Handle imports

	for(auto const& imp : module->imports)
	{
		imports.push_back(context.getModule(imp.name));
	}

	/*
	for(auto& typeDef : module->typeDefs)
	{
		typeDef.second.doResolve(*this);
	}
	*/

	for(auto const& func : module->functions)
	{
		func.second->doResolve();
	}

	for(auto const& func : module->functions)
	{
		if(func.second->body)
		{
			vector<llvm::Type*> ArgTys;
			for(auto& param : func.second->parameters)
			{
				ArgTys.push_back(param->declType->llvmType(*this));
			}

			llvm::Function* compiledFunc = llvm::cast<llvm::Function>(M->getOrInsertFunction(
				func.second->name,
				llvm::FunctionType::get(func.second->returnType->llvmType(*this), ArgTys, false),
				llvm::AttrListPtr::get((llvm::AttributeWithIndex *)0, 0)));

			func.second->compiledFunc = compiledFunc;
		}
	}

	abortFunc = llvm::cast<llvm::Function>(M->getOrInsertFunction("abort", llvm::Type::getVoidTy(llvmContext), (llvm::Type *)0));
}

void ModuleBuilder::buildAst()
{
	assert(M);

	for(auto const& func : module->functions)
	{
		if(func.second->body)
		{
			llvm::Function* compiledFunc = func.second->compiledFunc;
			llvm::BasicBlock* entryBb = llvm::BasicBlock::Create(llvmContext, "EntryBlock", compiledFunc);

			FuncBuilder builder(*this, entryBb, compiledFunc);
		
			// TODO: Move this whole code into FuncDef::build
			func.second->build(builder);

			llvm::Value* ret = func.second->body->build(builder);

			if(func.second->returnType->kind != TyUnit)
			{
				builder.ir.CreateRet(ret);
			}
			else
			{
				builder.ir.CreateRetVoid();
			}
		}
	}

	llvm::outs() << "We just constructed this LLVM module:\n\n" << *M;
}

void Parser::next(bool se)
{
tailcall:
	char curch = ch;
	nextch();
	switch(curch)
	{
		case '{': token = TLBrace; break;
		case '}': token = TRBrace; break;
		case '(': token = TLParen; break;
		case ')': token = TRParen; break;
		case ';': token = TSemicolon; break;
		case '#': token = THash; break;
		case '+': token = TPlus; break;
		case '-':
			if (ch == '-')
			{
				nextch();
				if(ch == '[' && (nextch(), ch == '['))
				{
					do
						nextch();
					while ((ch != ']' || (nextch(), ch != ']')) && ch != 0);

					nextch();
				}
				else
				{
					do
						nextch();
					while (ch != '\n' && ch != 0);
				}

				goto tailcall;
			}
			else
				token = TMinus;

			break;
		case ',': token = TComma; break;
		case '<': token = TLessThan; break;
		case '.': token = TDot; break;
		case '=':
			if (ch == '=')
			{
				nextch();
				token = TDblEqual;
			}
			else
				token = TEqual;

			break;
		case 0: token = TEof; break;

		case ' ': case '\t': case '\r': case '\n':
			if (curch == '\n')
				++currentLine;
			if (curch == '\n' && se)
				token = TSemicolon;
			else
				goto tailcall;
			break;

		default:
		{
			tokenStr.clear();

			if (curch >= 'a' && curch <= 'z')
			{
				tokenStr.push_back(curch);

				while ((ch >= 'a' && ch <= 'z')
					|| (ch >= 'A' && ch <= 'Z')
					|| (ch >= '0' && ch <= '9')
					|| (ch == '_'))
				{
					tokenStr.push_back(ch);
					nextch();
				}

				auto kw = keywords.find(tokenStr);
				if (kw == keywords.end())
					token = TIdent;
				else
					token = kw->second;
			}
			else if (curch >= 'A' && curch <= 'Z')
			{
				tokenStr.push_back(curch);

				while ((ch >= 'a' && ch <= 'z')
					|| (ch >= 'A' && ch <= 'Z')
					|| (ch >= '0' && ch <= '9')
					|| (ch == '_'))
				{
					tokenStr.push_back(ch);
					nextch();
				}

				token = TTypeIdent;
			}
			else if (curch >= '0' && curch <= '9')
			{
				tokenStr.push_back(curch);

				while ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e')
				{
					tokenStr.push_back(ch);
					nextch();
				}

				token = TConstNum;
			}
			else
			{
				throw CompileError(CEUnexpectedCharacter, currentLine);
			}
		}
	}
}

IntrusiveRefCntPtr<VarDecl> Parser::resolveLocal(
	IntrusiveRefCntPtr<Scope> sc,
	string const& name)
{
	if (!sc) return IntrusiveRefCntPtr<VarDecl>();
	return sc->resolveLocal(name);
}

ExprPtr ModuleBuilder::resolveIdent(string const& name)
{
	auto i = module->functions.find(name);
	if(i != module->functions.end())
		return ExprPtr(new FuncRefExpr(i->second));

	// Check imports
	for(auto const& imp : imports)
	{
		auto t = imp->module->functions.find(name);
		if(t != imp->module->functions.end())
		{
			return ExprPtr(new FuncRefExpr(t->second));
		}
	}

	return ExprPtr();
}

TypePtr Parser::resolveIdentType(string const& name)
{
	if (!currentTypeScope) return TypePtr();
	return currentTypeScope->resolveIdent(name);
}

ExprPtr Parser::block(bool se)
{
	expect(TLBrace);
	auto ret = statements();
	expect(TRBrace, se);
	return ret;
}

ExprPtr Parser::controlSeqExpression(bool se)
{
	if(test(TIf))
	{
		auto ifExpr = enterScope<IfExpr>(new IfExpr);
		ifExpr->cond = expression(false);
		enterScope(new Scope);
		ifExpr->trueCase = exitScope<Expr>(controlSeqExpression(se));
		exitScope<IfExpr>(ExprPtr());
		ExprPtr ret = ifExpr;
		if(test(TElse))
		{
			enterScope(new Scope);
			auto falseCase = exitScope<Expr>(controlSeqExpression(se));
			ret = inScope(new IfElseExpr(ifExpr, falseCase));
		}
		return ret;
	}
	/* TODO
	else if (Test(Token.While))
	{
		var whileExpr = EnterScopeT<WhileExpr>();

		whileExpr.Cond = Expression(false);
		whileExpr.Body = ControlSeqExpression(se);
		ExitScopeT<WhileExpr>(null);
		return whileExpr;
	}*/
	else if(token == TLBrace)
	{
		return block(se);
	}

	assert(false);
	return ExprPtr();
}

TypeDataDef& TypeConstructor::typeData()
{
	assert(type->kind == TyDef);
	return static_cast<TypeDataDef&>(*type);
}

void PatternBindingExpr::doResolve(
	ModuleBuilder& modBuilder,
	TypePtr const& valueType)
{
	if(!var->declType)
		var->declType = valueType;
	var->declType = var->declType->doResolve(modBuilder);
	
	if(! var->declType->isAssignableFrom(*valueType))
		throw CompileError(CETypeError, -1);
}

bool PatternBindingExpr::build(
	FuncBuilder& builder,
	llvm::Value* value,
	llvm::BasicBlock* matchOk,
	llvm::BasicBlock* matchFail)
{
	// Non-rejecting
	var->buildInit(builder, value);
	if(matchOk)
		builder.ir.CreateBr(matchOk);
	return true;
}

void PatternNamedCtorExpr::doResolve(
	ModuleBuilder& modBuilder,
	TypePtr const& valueType)
{
	// TODO: ctor->type may be generic, e.g. Box(a).
	// We need to match this against valueType, which may be e.g. Box(Int)
	// translating ctor->members[i].type to 'Int', not 'a'.

	auto ctorI = modBuilder.module->constructorDefs.find(name);
	if(ctorI == modBuilder.module->constructorDefs.end())
		throw CompileError(CEUnresolvedIdentifier, -1);
	ctor = ctorI->second;

	if(! ctor->type->isAssignableFrom(*valueType))
		throw CompileError(CETypeError, -1);

	if(subPatterns.size() != (size_t)ctor->ctorParams)
		throw CompileError(CEGeneralError, -1);

	for(auto i = 0u; i < subPatterns.size(); ++i)
	{
		subPatterns[i]->doResolve(modBuilder, ctor->members[i].type);
	}
}

bool PatternNamedCtorExpr::build(
	FuncBuilder& builder,
	llvm::Value* value,
	llvm::BasicBlock* matchOk,
	llvm::BasicBlock* matchFail)
{
	auto& td = ctor->typeData();

	bool childrenNonReject = true;

	if(!matchOk && td.def->constructors.size() == 1)
	{
		auto coercedValue = builder.ir.CreateBitCast(value, ctor->getLlvmType(builder.modBuilder));

		for(int i = 0; i < ctor->ctorParams; ++i)
		{
			auto sourceSlot = builder.ir.CreateStructGEP(coercedValue, i + 2);
			auto source = builder.ir.CreateLoad(sourceSlot);
			
			bool nonReject = subPatterns[i]->build(builder, source, 0, 0);

			assert(nonReject);
		}
	}
	else
	{
		if(!matchOk)
			throw CompileError(CEGeneralError, -1);

		auto valueTagSlot = builder.ir.CreateStructGEP(value, 1);
		auto valueTag = builder.ir.CreateLoad(valueTagSlot);
		auto tagCheck = builder.ir.CreateICmpEQ(valueTag, builder.ir.getInt32(ctor->tag));

		auto tagOkBB = llvm::BasicBlock::Create(builder.modBuilder.llvmContext, "then", builder.func);

		// TODO: Deconstruct live variables on match fail
		builder.ir.CreateCondBr(tagCheck, tagOkBB, matchFail);

		builder.ir.SetInsertPoint(tagOkBB);
		auto coercedValue = builder.ir.CreateBitCast(value, ctor->getLlvmType(builder.modBuilder));

		for(int i = 0; i < ctor->ctorParams; ++i)
		{
			auto subOkBB = llvm::BasicBlock::Create(builder.modBuilder.llvmContext, "then", builder.func);

			auto sourceSlot = builder.ir.CreateStructGEP(coercedValue, i + 2);
			auto source = builder.ir.CreateLoad(sourceSlot);
			
			// TODO: Deconstruct live variables on match fail
			bool nonReject = subPatterns[i]->build(builder, source, subOkBB, matchFail);

			childrenNonReject = childrenNonReject && nonReject;

			builder.ir.SetInsertPoint(subOkBB);
		}

		builder.ir.CreateBr(matchOk);
	}

	return childrenNonReject && (td.def->constructors.size() == 1);
}

ExprPtr MatchSingleNonRejectExpr::resolve(ModuleBuilder& modBuilder)
{
	value = value->doResolve(modBuilder);
	pattern->doResolve(modBuilder, value->type);
	type = tyUnit;
	return ExprPtr(this);
}

llvm::Value* MatchSingleNonRejectExpr::build(FuncBuilder& builder)
{
	auto valueVal = value->build(builder);

	bool nonReject = pattern->build(builder, valueVal, 0, 0);

	if(!nonReject)
		throw CompileError(CEGeneralError, this);

	return 0;
}

llvm::Value* MatchExpr::build(FuncBuilder& builder)
{
	auto valueVal = value->build(builder);

	auto failBB = llvm::BasicBlock::Create(builder.modBuilder.llvmContext, "fail", builder.func);

	auto endBB = llvm::BasicBlock::Create(builder.modBuilder.llvmContext, "end", builder.func);
	llvm::BasicBlock* nextBB;

	vector<llvm::BasicBlock*> BBs;
	vector<llvm::Value*> values;

	bool complete = false;

	for(auto i = 0u; i < legs.size(); ++i)
	{
		if(i + 1 < legs.size())
			nextBB = llvm::BasicBlock::Create(builder.modBuilder.llvmContext, "tryNext", builder.func);
		else
			nextBB = failBB;

		auto matchOkBB = llvm::BasicBlock::Create(builder.modBuilder.llvmContext, "success", builder.func);

		bool nonReject = legs[i].pattern->build(builder, valueVal, matchOkBB, nextBB);

		if(nonReject)
		{
			// Matching is complete if it ends with
			// a non-rejecting pattern
			complete = true;
		}

		builder.ir.SetInsertPoint(matchOkBB);

		auto val = legs[i].body->build(builder);

		BBs.push_back(matchOkBB);
		values.push_back(val);

		builder.ir.CreateBr(endBB);

		if(nonReject && i + 1 < legs.size())
		{
			throw CompileError(CEUnreachablePattern, this);
		}
			
		builder.ir.SetInsertPoint(nextBB);
	}

	if(!complete)
	{
		// Not complete. We need a fall-back case.
		builder.ir.SetInsertPoint(failBB);
		builder.ir.CreateCall(builder.modBuilder.abortFunc, "");
		builder.ir.CreateUnreachable();
	}

	builder.ir.SetInsertPoint(endBB);

	if(type->kind != TyUnit)
	{
		if(BBs.size() > 1)
		{
			llvm::PHINode* phi = builder.ir.CreatePHI(type->llvmType(builder.modBuilder), BBs.size());

			for(auto i = 0u; i < BBs.size(); ++i)
			{
				phi->addIncoming(values[i], BBs[i]);
			}

			return phi;
		}
		else
		{
			// Single value
			return values[0];
		}
	}
	else
	{
		return 0;
	}
}

ExprPtr MatchExpr::resolve(ModuleBuilder& modBuilder)
{
	value = value->doResolve(modBuilder);
		
	for(auto& leg : legs)
	{
		leg.pattern->doResolve(modBuilder, value->type);
		leg.body = leg.body->doResolve(modBuilder);

		if(! type)
			type = leg.body->type;
		else
			type = type->commonWith(leg.body->type);
	}

	return ExprPtr(this);
}

IntrusiveRefCntPtr<PatternExpr> Parser::pattern(bool se)
{
	// TODO: Proper pattern syntax
	if(token == TTypeIdent)
	{
		string const& ctorName = nextIdent(se);

		IntrusiveRefCntPtr<PatternNamedCtorExpr> pt(
			new PatternNamedCtorExpr());
		pt->name = ctorName;

		if(test(TLParen))
		{
			do
			{
				pt->subPatterns.push_back(pattern(false));
			}
			while(test(TComma));

			expect(TRParen);
		}

		return IntrusiveRefCntPtr<PatternExpr>(pt.getPtr());
	}
	else if(token == TIdent)
	{
		string const& bindingName = nextIdent(se);

		auto varDecl = declVar(bindingName, TypePtr());

		IntrusiveRefCntPtr<PatternBindingExpr> pt(
			new PatternBindingExpr());
		pt->var = varDecl;

		return IntrusiveRefCntPtr<PatternExpr>(pt.getPtr());
	}

	parseError();
	return IntrusiveRefCntPtr<PatternExpr>();
}

void Parser::varDecl(SeqExpr& seqExpr, bool se)
{
	expect(TLet);

	do
	{
		auto const& pat = pattern(false);

		expect(TEqual);
		auto const& val = expression(se);

		seqExpr.expressions.push_back(
			ExprPtr(new MatchSingleNonRejectExpr(val, pat)));
	}
	while(test(TComma));
}

ExprPtr Parser::patternMatch(bool se)
{
	expect(TCase);
	auto const& value = primaryExpression(false);
	expect(TLBrace);

	IntrusiveRefCntPtr<MatchExpr> matchExpr(new MatchExpr(value));

	do
	{
		if(token == TTypeIdent || token == TIdent) // TODO: firstPattern set
		{
			enterScope(new Scope);

			auto pt = pattern(false);
			
			// TODO: Allow pattern = expr
			auto body = exitScope<Expr>(block(true));

			matchExpr->legs.push_back(MatchLeg(pt, body));
		}
	}
	while(test(TSemicolon));

	expect(TRBrace, se);

	return ExprPtr(matchExpr.getPtr()); // TEMP
}

ExprPtr Parser::primaryExpression(bool se)
{
	ExprPtr ret;
	/* TODO
	if(test(TMinus))
	{
		return inScope(new UnaryExpr(PrimaryExpression(se), Token.Minus));
	}*/

	if(token == TCase)
	{
		ret = patternMatch(se);
	}
	else if (token == TIdent || token == TTypeIdent)
	{
		string const& name = nextIdent(se);

		auto varDecl = resolveLocal(currentScope, name);

		if(varDecl)
			ret = inScope(new VarRef(varDecl));
		else
			ret = inScope(new NamedRef(name));
	}
	else if(token == TConstNum)
	{
		if(tokenStr.find('.') != string::npos)
			ret = inScope(new FConstExpr(tokenNum(se)));
		else
			ret = inScope(new ConstExpr(tokenNum(se)));
	}
	else if(test(TLParen))
	{
		ret = expression(false);
		expect(TRParen);
	}
	else if (firstControlSeq[token])
	{
		ret = controlSeqExpression(se);
	}
	else
	{
		throw CompileError(CEParseError, currentLine);
	}

	while(true)
	{
		switch(token)
		{
			case TLParen:
			{
				next(false);

				auto call = inScope(new CallExpr());

				call->func = ret;
				ret = call;

				if(token != TRParen)
				{
					do
					{
						ExprPtr const& param = expression(false);
						call->parameters.push_back(param);
					}
					while(test(TComma));
				}

				expect(TRParen, se);
				break;
			}

			/*
			case Token.Dot:
			{
				Next(false);

				var name = ExpectIdent(se);

				ret = InScope(new SlotRef(ret, name));
				break;
			}*/

			default:
				return ret;
		}
	}
}

ExprPtr Parser::expressionRest(ExprPtr lhs, int minpred, bool se)
{
	while(firstExpressionRest[token])
	{
		Token op = token;
		int pred = operatorPrecedence[op];
		if(pred < minpred)
			break;
		next(false);
		ExprPtr rhs(primaryExpression(se));

		while(firstExpressionRest[token])
		{
			Token op2 = token;
			int pred2 = operatorPrecedence[op2];
			if(pred2 <= pred)
				break;
			rhs = expressionRest(rhs, pred2, se);
		}

		lhs = inScope(new BinExpr(lhs, rhs, op));
	}

	return lhs;
}

ExprPtr Parser::expression(bool se)
{
	return expressionRest(primaryExpression(se), 0, se);
}

void Parser::statement(SeqExpr& seqExpr)
{
	if(token == TLet)
	{
		varDecl(seqExpr, true);
	}
	else if(firstExpression[token])
	{
		seqExpr.expressions.push_back(expression(true));
	}
}

ExprPtr Parser::statements()
{
	unique_ptr<SeqExpr> seq(inScope(new SeqExpr()));

	do
	{
#if 0 // TODO. Disabled until we know how to handle return
		if (test(TReturn))
		{
			seq->expressions.push_back(ExprPtr(inScope(
				new ReturnExpr(expression(true)))));
			break; // Return must be the last statement in a statement list
		}
#endif
					
		statement(*seq);
	}
	while(test(TSemicolon));

	return ExprPtr(seq.release());
}

void Parser::expect(Token t, bool se)
{
	if(token != t)
		throw CompileError(CEUnexpectedToken, currentLine);
	next(se);
}

void Parser::expectPeek(Token t)
{
	if(token != t)
		throw CompileError(CEUnexpectedToken, currentLine);
}

string Parser::expectIdent(bool se)
{
	if(token != TIdent)
		throw CompileError(CEUnexpectedToken, currentLine);
	string ret(tokenStr);
	next(se);

	return ret;
}

string Parser::nextIdent(bool se)
{
	string ret(tokenStr);
	next(se);

	return ret;
}

bool Parser::test(Token t, bool se)
{
	if(token == t)
	{
		next(se);
		return true;
	}

	return false;
}

llvm::PointerType* TypeDataDef::llvmType(ModuleBuilder& /*modBuilder*/)
{
	assert(def->resolved);

	if(!def->llvmType)
	{
		// Set first to avoid infinite recursion
		def->llvmType = modBuilder->context.typeTaggedObjPtr;

		for(auto const& c : def->constructors)
		{
			(void)c->getLlvmType(*modBuilder);
		}
	}

	return def->llvmType;
}
