#include "pir_tests.h"
#include "R/Protect.h"
#include "R_ext/Parse.h"
#include "analysis/query.h"
#include "analysis/verifier.h"
#include "ir/Compiler.h"
#include "pir/pir_impl.h"
#include "translations/rir_2_pir.h"
#include "util/visitor.h"

namespace {
using namespace rir;

std::pair<pir::Closure*, pir::Module*> compile(const std::string& inp,
                                               SEXP env = R_GlobalEnv) {
    Protect p;
    ParseStatus status;
    SEXP arg = p(CONS(R_NilValue, R_NilValue));
    SET_TAG(arg, Rf_install("arg1"));
    SEXP str = p(Rf_mkString(inp.c_str()));
    SEXP bdy = p(R_ParseVector(str, -1, &status, R_NilValue));
    SEXP fun = p(Compiler::compileClosure(CDR(bdy), arg));
    CLOENV(fun) = env;
    pir::Module* m = new pir::Module;
    pir::Rir2PirCompiler cmp(m);
    // cmp.setVerbose(true);
    auto f = cmp.compileClosure(fun);
    cmp.optimizeModule();
    return std::pair<pir::Closure*, pir::Module*>(f, m);
}

using namespace rir::pir;
typedef std::function<bool(void)> TestClosure;
typedef std::pair<std::string, TestClosure> Test;

#define CHECK(___test___)                                                      \
    if (!(___test___)) {                                                       \
        m->print(std::cerr);                                                   \
        std::cerr << "'" << #___test___ << "' failed\n";                       \
        assert(false);                                                         \
        return false;                                                          \
    }

bool test42(const std::string& input) {
    auto res = compile(input);
    auto f = res.first;
    auto m = res.second;

    CHECK(Query::noEnv(f));

    auto r = Query::returned(f);
    CHECK(r.size() == 1);

    auto ld = LdConst::Cast((*r.begin()));
    CHECK(ld);
    CHECK(TYPEOF(ld->c) == INTSXP);
    CHECK(INTEGER(ld->c)[0] == 42);
    delete m;
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

bool compileAndVerify(const std::string& input) {
    auto m = compile(input).second;
    bool t = verify(m);
    delete m;
    return t;
}

bool canRemoveEnvironment(const std::string& input) {
    auto r = compile(input);
    auto m = r.second;
    auto f = r.first;
    bool t = verify(m);
    t = t && Query::noEnv(f);
    delete m;
    return t;
}

bool testDelayEnv() {
    // TODO: counterexample: closure creates circular dependency, need more
    //       analysis!
    // auto m = compile("{f <- function()1; arg1[[2]]}");

    auto res = compile("{f <- arg1; arg1[[2]]}");
    auto f = res.first;
    auto m = res.second;
    bool t = Visitor::check(f->entry, [&](Instruction* i, BB* bb) {
        if (i->hasEnv())
            CHECK(Deopt::Cast(bb->last()));
        return true;
    });
    delete m;
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
        // This super assign can be fully resolved, and dead store eliminated
        auto r = compile("{a <- 1; (function() a <<- 1)()}");
        auto m = r.second;
        auto f = r.first;
        CHECK(!hasSuperAssign(f));
        CHECK(!hasAssign(f));
        delete m;
    }
    {
        // This super assign can be converted into a store to the global env
        auto r = compile("{(function() a <<- 1)()}");
        auto m = r.second;
        auto f = r.first;
        CHECK(!hasSuperAssign(f));
        CHECK(hasAssign(f));
        delete m;
    }
    {
        // This super assign cannot be removed, since the super env is not
        // assigneable.
        auto r = compile("{f <- (function() a <<- 1)()}", R_NilValue);
        auto m = r.second;
        auto f = r.first;
        CHECK(hasSuperAssign(f));
        delete m;
    }

    return true;
}

static Test tests[] = {
    Test("test_42L", []() { return test42("42L"); }),
    Test("test_inline", []() { return test42("{f <- function() 42L; f()}"); }),
    Test("return_cls", []() { return compileAndVerify("function() 42L"); }),
    Test("index", []() { return compileAndVerify("arg1[[2]]"); }),
    Test("test_inline_arg",
         []() { return test42("{f <- function(x) x; f(42L)}"); }),
    Test("test_assign",
         []() { return test42("{y<-42L; if (arg1) x<-y else x<-y; x}"); }),
    Test(
        "test_super_assign",
        []() { return test42("{x <- 0; f <- function() x <<- 42L; f(); x}"); }),
    Test("return_cls", []() { return compileAndVerify("function() 42L"); }),
    Test("index", []() { return compileAndVerify("arg1[[2]]"); }),
    Test("deopt_in_prom",
         []() {
             return compileAndVerify(
                 "{function(a) {f <- function(x) x; f(a[[1]])}}");
         }),
    Test("delay_env", &testDelayEnv),
    Test("context_load", []() { return canRemoveEnvironment("a"); }),
    Test("super_assign", &testSuperAssign),
    Test("loop",
         []() {
             return compileAndVerify(
                 "{function(x) {while (x < 10) if (x) x <- x + 1}}");
         }),
};
}

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
}
