#include "pir_tests.h"
#include "R/Protect.h"
#include "R/RList.h"
#include "R_ext/Parse.h"
#include "analysis/query.h"
#include "analysis/verifier.h"
#include "ir/Compiler.h"
#include "pir/pir_impl.h"
#include "translations/pir_2_rir.h"
#include "translations/rir_2_pir/rir_2_pir.h"
#include "util/visitor.h"
#include <string>
#include <vector>

namespace {
using namespace rir;

std::unordered_map<std::string, pir::Closure*>
compile(const std::string& context, const std::string& expr, pir::Module* m,
        SEXP super = R_GlobalEnv) {
    Protect p;

    auto eval = [&](const std::string& expr, SEXP env) {
        ParseStatus status;
        // Parse expression
        SEXP str = p(Rf_mkString(("{" + expr + "}").c_str()));
        SEXP e = p(R_ParseVector(str, -1, &status, R_NilValue));

        // Compile expression to rir
        SEXP rirexp =
            p(Compiler::compileFunction(VECTOR_ELT(e, 0), R_NilValue));

        // Evaluate expression under the fresh environment `env`
        Rf_eval(rirexp, env);
    };

    if (context != "") {
        SEXP contextEnv = p(Rf_allocSExp(ENVSXP));
        ENCLOS(contextEnv) = super;
        eval(context, contextEnv);
        super = contextEnv;
    }

    SEXP env = p(Rf_allocSExp(ENVSXP));
    ENCLOS(env) = super;
    eval(expr, env);

    pir::Rir2PirCompiler cmp(m);

    // Compile every function the expression created
    std::unordered_map<std::string, pir::Closure*> results;
    auto envlist = RList(FRAME(env));
    for (auto f = envlist.begin(); f != envlist.end(); ++f) {
        auto fun = *f;
        if (TYPEOF(fun) == CLOSXP) {
            assert(isValidClosureSEXP(fun));
            // isValidClosureSEXP(fun)->body()->print();
            results[CHAR(PRINTNAME(f.tag()))] = cmp.compileClosure(fun);
        }
    }

    m->print(std::cout);
    // cmp.setVerbose(true);
    cmp.optimizeModule();
    return results;
}

using namespace rir::pir;
typedef std::function<bool(void)> TestClosure;
typedef std::pair<std::string, TestClosure> Test;

#define CHECK(___test___)                                                      \
    if (!(___test___)) {                                                       \
        m.print(std::cerr);                                                    \
        std::cerr << "'" << #___test___ << "' failed\n";                       \
        assert(false);                                                         \
        return false;                                                          \
    }

bool test42(const std::string& input) {
    pir::Module m;
    auto res = compile("", "theFun <- function() " + input, &m);
    auto f = res["theFun"];

    CHECK(Query::noEnv(f));

    auto r = Query::returned(f);
    CHECK(r.size() == 1);

    auto ld = LdConst::Cast((*r.begin()));
    CHECK(ld);
    CHECK(TYPEOF(ld->c) == INTSXP);
    CHECK(INTEGER(ld->c)[0] == 42);
    return true;
};

class NullBuffer : public std::ostream {
  public:
    int overflow(int c) { return c; }
};

bool verify(Module* m) {
    bool success = true;
    m->eachPirFunction([&success](pir::Module::VersionedClosure& f) {
        f.eachVersion([&success](pir::Closure* f) {
            if (!Verify::apply(f))
                success = false;
        });
    });
    // TODO: find fix for osx
    // NullBuffer nb;
    m->print(std::cout);

    return true;
}

bool compileAndVerify(const std::string& context, const std::string& input) {
    pir::Module m;
    compile(context, input, &m);
    bool t = verify(&m);
    return t;
}

bool compileAndVerify(const std::string& input) {
    return compileAndVerify("", input);
}

bool canRemoveEnvironment(const std::string& input) {
    pir::Module m;
    compile("", input, &m);
    bool t = verify(&m);
    m.eachPirFunction([&t](pir::Closure* f) { t = t && Query::noEnv(f); });
    return t;
}

bool testDelayEnv() {
    // TODO: counterexample: closure creates circular dependency, need more
    //       analysis!
    // auto m = compile("{f <- function()1; arg1[[2]]}");

    pir::Module m;
    auto res = compile("", "a <- function(b) {f <- b; b[[2]]}", &m);
    bool t = Visitor::check(res["a"]->entry, [&](Instruction* i, BB* bb) {
        if (i->hasEnv())
            CHECK(Deopt::Cast(bb->last()));
        return true;
    });
    return t;
}

extern "C" SEXP Rf_NewEnvironment(SEXP, SEXP, SEXP);
bool testSuperAssign() {
    auto hasAssign = [](pir::Closure* f) {
        return !Visitor::check(f->entry, [&](Instruction* i) {
            if (StVar::Cast(i))
                return false;
            return true;
        });
    };
    auto hasSuperAssign = [](pir::Closure* f) {
        return !Visitor::check(f->entry, [&](Instruction* i) {
            if (StVarSuper::Cast(i))
                return false;
            return true;
        });
    };
    {
        pir::Module m;
        // This super assign can be fully resolved, and dead store eliminated
        auto res =
            compile("", "f <- function() {a <- 1; (function() a <<- 1)()}", &m);
        auto f = res["f"];
        CHECK(!hasSuperAssign(f));
        CHECK(!hasAssign(f));
    }
    {
        pir::Module m;
        // This super assign can be converted into a store to the global env
        auto res = compile("", "f <- function() {(function() a <<- 1)()}", &m);
        auto f = res["f"];
        CHECK(!hasSuperAssign(f));
        CHECK(hasAssign(f));
    }
    {
        pir::Module m;
        // This super assign cannot be removed, since the super env is not
        // unknown.
        auto res = compile(
            "", "f <- function() {f <- (function() {qq(); a <<- 1})()}", &m);
        auto f = res["f"];
        CHECK(hasSuperAssign(f));
    }

    return true;
}

// ----------------- PIR to RIR tests -----------------

SEXP parseCompileToRir(std::string input) {
    Protect p;
    ParseStatus status;

    SEXP str = p(Rf_mkString(input.c_str()));
    SEXP expr = p(R_ParseVector(str, -1, &status, R_NilValue));
    SEXP cls = p(Rf_eval(VECTOR_ELT(expr, 0), R_GlobalEnv));

    return Compiler::compileClosure(BODY(cls), FORMALS(cls));
}

SEXP createRWrapperCall(std::string input) {
    Protect p;
    ParseStatus status;

    std::string wrapper = "rir.compile( function() " + input + " )()";

    Rprintf("   > %s\n\n", wrapper.c_str());

    SEXP str = p(Rf_mkString(wrapper.c_str()));
    SEXP expr = p(R_ParseVector(str, -1, &status, R_NilValue));
    SEXP call = p(VECTOR_ELT(expr, 0));

    return call;
}

bool checkPir2Rir(SEXP expected, SEXP result) {
    if (expected == result)
        return true;
    if (TYPEOF(expected) != TYPEOF(result))
        return false;
    if (XLENGTH(expected) != XLENGTH(result))
        return false;
    for (size_t i = 0; i < XLENGTH(expected); ++i) {
        switch (TYPEOF(expected)) {
        case INTSXP:
            if (INTEGER(expected)[i] != INTEGER(result)[i])
                return false;
            break;
        case REALSXP:
            if (REAL(expected)[i] != REAL(result)[i])
                return false;
            break;
        default:
            assert(false);
        }
    }
    return true;
}

extern "C" SEXP rir_eval(SEXP, SEXP);
extern "C" SEXP pir_compile(SEXP);

bool testPir2Rir(std::string name, std::string fun, std::string args) {
    Protect p;

    std::string call = name + "(" + args + ")";

    Rprintf("   > %s <- %s\n", name.c_str(), fun.c_str());

    auto execEnv = p(Rf_NewEnvironment(R_NilValue, R_NilValue, R_GlobalEnv));
    auto rirFun = p(parseCompileToRir(fun));
    SET_CLOENV(rirFun, execEnv);
    Rf_defineVar(Rf_install(name.c_str()), rirFun, execEnv);
    auto rCall = createRWrapperCall(call);

    auto orig = p(Rf_eval(rCall, execEnv));
    Rprintf(" orig = %p\n", orig);
    Rf_PrintValue(orig);

    pir_compile(rirFun);

    auto after = p(Rf_eval(rCall, execEnv));
    Rprintf("after = %p\n", after);
    Rf_PrintValue(after);

    return checkPir2Rir(orig, after);
}

static Test tests[] = {
    Test("test_42L", []() { return test42("42L"); }),
    Test("test_inline", []() { return test42("{f <- function() 42L; f()}"); }),
    Test("test_inline", []() { return test42("{f <- function() 42L; f()}"); }),
    Test("test_inline_two",
         []() {
             return test42(
                 "{f <- function(val, fun) fun(val); f(42L, function(x)x)}");
         }),
    Test("test_inline_arg",
         []() { return test42("{f <- function(x) x; f(42L)}"); }),
    Test("test_assign",
         []() { return test42("{y<-42L; if (arg1) x<-y else x<-y; x}"); }),
    Test(
        "test_super_assign",
        []() { return test42("{x <- 0; f <- function() x <<- 42L; f(); x}"); }),

    Test("return_cls",
         []() { return compileAndVerify("f <- function() 42L"); }),
    Test("index", []() { return compileAndVerify("f <- function(x) x[[2]]"); }),
    Test("return_cls",
         []() { return compileAndVerify("f <- function() {function() 42L}"); }),
    Test("deopt_in_prom",
         []() {
             return compileAndVerify(
                 "fun <- function(a) {f <- function(x) x; f(a[[1]])}");
         }),
    Test("delay_env", &testDelayEnv),
    Test("context_load",
         []() { return canRemoveEnvironment("f <- function() a"); }),
    Test("super_assign", &testSuperAssign),
    Test("loop",
         []() {
             return compileAndVerify(
                 "f <- function(x) {while (x < 10) if (x) x <- x + 1}");
         }),
    Test("static_call",
         []() {
             return compileAndVerify("f <- function(x) x",
                                     "g <- function() f(1); g()");
         }),
    Test("PIR to RIR: basic",
         []() { return testPir2Rir("foo", "function() 42L", ""); }),
    Test("PIR to RIR: simple argument",
         []() { return testPir2Rir("foo", "function(x) x", "16L"); }),
    // Test("PIR to RIR: default arg",
    //      []() { return testPir2Rir("foo", "function(x = 3) x", ""); }),
    Test("PIR to RIR: local binding",
         []() {
             return testPir2Rir("foo",
                                "function(dummy, a) { x <- 3; x + a + x + a }",
                                "cat('WHOA\n'), 1");
         }),
    Test("PIR to RIR: if",
         []() {
             return testPir2Rir("foo", "function(x) if (x) 1 else 2", "TRUE");
         }),
    Test("PIR to RIR: if",
         []() { return testPir2Rir("foo", "function(x) if (x) 1", "F"); }),
    Test("PIR to RIR: simple loop",
         []() {
             return testPir2Rir("foo", "function(x) while (TRUE) if (x) break",
                                "T");
         }),
    Test("PIR to RIR: loop",
         []() {
             return testPir2Rir("foo", "function(x) {"
                                       "  sum <- 0;"
                                       "  while (x > 0) {"
                                       "    sum <- sum + x;"
                                       "    x <- x - 1"
                                       "  };"
                                       "  sum"
                                       "}",
                                "10");
         }),
    Test("PIR to RIR: loop with break and next",
         []() {
             return testPir2Rir("foo", "f <- function(x, y) {\n"
                                       "    s <- 0L\n"
                                       "    repeat {\n"
                                       "        if (x > y)\n"
                                       "            break\n"
                                       "        if (x %% 2L == 1L) {\n"
                                       "            x <- x + 1L\n"
                                       "        } else {\n"
                                       "            x <- x + 1L\n"
                                       "            y <- y - 1L\n"
                                       "            next\n"
                                       "        }\n"
                                       "        s <- s + x\n"
                                       "    }\n"
                                       "    s\n"
                                       "}",
                                "1L, 10L");
         }),
    // TODO: fails w/ "Cannot cast val to int$" for the loop index
    // Test("PIR to RIR: simple for loop",
    //      []() {
    //          return testPir2Rir("foo",
    //                             "function(x) { s = 0; for (i in 1:x) s = s +
    //                             i; s }",
    //                             "10L");
    //      }),
    // function(x) foo(x)
};
} // namespace

namespace rir {

void PirTests::run() {
    for (auto t : tests) {
        std::cout << "> " << t.first << "\n";
        if (!t.second()) {
            std::cout << "failed\n";
            exit(1);
        }
    }
}
} // namespace rir
