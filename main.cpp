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
#include <streambuf>

using namespace llvm;

// For testing
struct Foo
{
	int rc;
	int tag;
	double _1;
};

IntrusiveRefCntPtr<ModuleBuilder> Context::compileModule(
	std::istream& in)
{
	std::string str((std::istreambuf_iterator<char>(in)),
					 std::istreambuf_iterator<char>());

	Parser parser(str.data(), str.data() + str.size());

	auto mod = parser.module();

	auto modBuilder = addModule(mod);

	return modBuilder;
}

int main()
{
	InitializeNativeTarget();

	std::ifstream test("Test.mlk");

	Context ctx;
	
	auto modBuilder = ctx.compileModule(test);

	ctx.doResolve();

	for(auto const& mod : ctx.modules)
	{
		mod.second->buildAst();
	}

#if 1
	unique_ptr<llvm::ExecutionEngine> EE(llvm::EngineBuilder(modBuilder->M).create());

	for(auto const& otherMod : ctx.modules)
	{
		if(otherMod.second != modBuilder)
		{
			EE->addModule(otherMod.second->M);
		}
	}

	Function* f = modBuilder->M->getFunction("main");
	
	vector<llvm::GenericValue> args;
	llvm::GenericValue gv = EE->runFunction(f, args);

	llvm::outs() << "RC: " << ((Foo*)gv.PointerVal)->rc << "\n";
	llvm::outs() << "Tag: " << ((Foo*)gv.PointerVal)->tag << "\n";
	llvm::outs() << "Result: " << ((Foo*)gv.PointerVal)->_1 << "\n";
	EE->freeMachineCodeForFunction(f);
#endif
      
	llvm_shutdown();
	return 0;
}
