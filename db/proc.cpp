/*
 * Copyright (C) 1997-2001, The University of Queensland
 * Copyright (C) 2000-2001, Sun Microsystems, Inc
 * Copyright (C) 2002-2006, Trent Waddington and Mike Van Emmerik
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 */

/***************************************************************************//**
 * \file    proc.cpp
 * \brief   Implementation of the Proc hierachy (Proc, UserProc, LibProc).
 *               All aspects of a procedure, apart from the actual code in the
 *               Cfg, are stored here
 *
 * Copyright (C) 1997-2001, The University of Queensland, BT group
 * Copyright (C) 2000-2001, Sun Microsystems, Inc
 ******************************************************************************/

/*
 * $Revision$    // 1.238.2.44
 *
 * 14 Mar 02 - Mike: Fixed a problem caused with 16-bit pushes in richards2
 * 20 Apr 02 - Mike: Mods for boomerang
 * 31 Jan 03 - Mike: Tabs and indenting
 * 03 Feb 03 - Mike: removeStatement no longer linear searches for the BB
 * 13 Jul 05 - Mike: Fixed a segfault in processDecodedICTs with zero lentgth (!) BBs. Also one in the ad-hoc TA
 * 08 Mar 06 - Mike: fixed use of invalidated iterator in set/map::erase() (thanks, tamlin!)
 */
/***************************************************************************//**
 * \class Proc
 *
 * \var Proc::visited
 * \brief For printCallGraphXML
 * \var Proc::prog
 * \brief Program containing this procedure.
 * \var Proc::signature
 * \brief The formal signature of this procedure.
 * This information is determined
 * either by the common.hs file (for a library function) or by analysis.
 * NOTE: This belongs in the CALL, because the same procedure can have different
 * signatures if it happens to have varargs. Temporarily here till it can be permanently
 * moved.
 * \var Proc::address
 * Procedure's address.
 * \var Proc::m_firstCaller
 * first procedure to call this procedure.
 * \var Proc::m_firstCallerAddr
 * can only be used once.
 * \var Proc::provenTrue
 * All the expressions that have been proven true.
 * (Could perhaps do with a list of some that are proven false)
 * Proof the form r28 = r28 + 4 is stored as map from "r28" to "r28+4" (NOTE: no subscripts)
 * \var Proc::recurPremises
 * Premises for recursion group analysis. This is a preservation
 * that is assumed true only for definitions by calls reached in the proof. It also
 * prevents infinite looping of this proof logic.
 * \var Proc::callerSet
 * Set of callers (CallStatements that call this procedure).
 * \var Proc::cluster
 * Cluster this procedure is contained within.
 ******************************************************************************/
/******************************************************************************
 * Dependencies.
 ******************************************************************************/

#include <sstream>
#include <algorithm>        // For find()
#include <iomanip>            // For std::setw etc
#include <sstream>
#include <cstring>
#include "proc.h"
#include "types.h"
#include "type.h"
#include "cluster.h"
#include "statement.h"
#include "register.h"
#include "rtl.h"
#include "prog.h"
#include "hllcode.h"
#include "BinaryFile.h"
#include "frontend.h"
#include "util.h"
#include "signature.h"
#include "boomerang.h"
#include "constraint.h"
#include "visitor.h"
#include "log.h"

#ifdef _WIN32
#undef NO_ADDRESS
#include <windows.h>
#ifndef __MINGW32__
namespace dbghelp {
#include <dbghelp.h>
};
#endif
#undef NO_ADDRESS
#define NO_ADDRESS ADDRESS::g(-1)
#endif

typedef std::map<Statement*, int> RefCounter;

extern char debug_buffer[];        // Defined in basicblock.cpp, size DEBUG_BUFSIZE

/************************
 * Proc methods.
 ***********************/

Proc::~Proc()
{}

/***************************************************************************//**
 *
 * \brief        Constructor with name, native address.
 * \param        prg - the program this procedure belongs to
 * \param        uNative - Native address of entry point of procedure
 * \param        sig - the Signature for this Proc
 *
 ******************************************************************************/
Proc::Proc(Prog *prog, ADDRESS uNative, Signature *sig)
    : prog(prog), signature(sig), address(uNative), m_firstCaller(nullptr) {
    if (sig)
        cluster = prog->getDefaultCluster(sig->getName());
    else
        cluster = prog->getRootCluster();
}

/***************************************************************************//**
 *
 * \brief        Returns the name of this procedure
 * PARAMETERS:        <none>
 * \returns            the name of this procedure
 ******************************************************************************/
const char* Proc::getName() const {
    assert(signature);
    return signature->getName();
}

/***************************************************************************//**
 *
 * \brief        Sets the name of this procedure
 * PARAMETERS:        new name
 *
 ******************************************************************************/
void Proc::setName(const std::string &nam) {
    assert(signature);
    signature->setName(nam);
}


/***************************************************************************//**
 *
 * \brief        Get the native address (entry point).
 * \returns            the native address of this procedure (entry point)
 ******************************************************************************/
ADDRESS Proc::getNativeAddress() const {
    return address;
}

/***************************************************************************//**
 *
 * \brief        Set the native address
 * \param a native address of the procedure
 *
 ******************************************************************************/
void Proc::setNativeAddress(ADDRESS a) {
    address = a;
}

bool LibProc::isNoReturn() {
    return FrontEnd::noReturnCallDest(getName());
}

/**
 * \var UserProc::cycleGrp
 * Pointer to a set of procedures involved in a recursion group.
 * \note Each procedure in the cycle points to the same set! However, there can be several separate cycles.
 * E.g. in test/source/recursion.c, there is a cycle with f and g, while another is being built up (it only
 * has c, d, and e at the point where the f-g cycle is found).
 * \var UserProc::stmtNumber
 * Current statement number. Makes it easier to split decompile() into smaller pieces.
 * \var UserProc::theReturnStatement
 * We ensure that there is only one return statement now. See code in frontend/frontend.cpp handling case
 * STMT_RET. If no return statement, this will be nullptr.
 * \var  UserProc::parameters
 * The list of parameters, ordered and filtered.
 * Note that a LocationList could be used, but then there would be nowhere to store the types (for DFA based TA)
 * The RHS is just ignored; the list is of ImplicitAssigns.
 * DESIGN ISSUE: it would be nice for the parameters' implicit assignments to be the sole definitions, i.e. not
 * need other implicit assignments for these. But the targets of RefExp's are not expected to change address,
 * so they are not suitable at present (since the addresses regularly get changed as the parameters get
 * recreated).
 * \var UserProc::col
 * A collector for initial parameters (locations used before being defined).  Note that final parameters don't
 * use this; it's only of use during group decompilation analysis (sorting out recursion)
 */

bool UserProc::isNoReturn() {
    // undecoded procs are assumed to always return (and define everything)
    if (!this->isDecoded())
        return false;

    BasicBlock * exitbb = cfg->getExitBB();
    if (exitbb == nullptr)
        return true;
    if (exitbb->getNumInEdges() == 1) {
        Statement *s = exitbb->getInEdges()[0]->getLastStmt();
        CallStatement *call = (CallStatement*)s;
        if (s->isCall() && call->getDestProc() && call->getDestProc()->isNoReturn())
            return true;
    }
    return false;
}

/***************************************************************************//**
 *
 * \brief Return true if this procedure contains the given address
 * \param uAddr address to search for
 * \returns          true if it does
 ******************************************************************************/
bool UserProc::containsAddr(ADDRESS uAddr) {
    BB_IT it;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it))
        if (bb->getRTLs() && bb->getLowAddr() <= uAddr && bb->getHiAddr() >= uAddr)
            return true;
    return false;
}

void Proc::renameParam(const char *oldName, const char *newName) {
    signature->renameParam(oldName, newName);
}

/**
 * Modify actuals so that it is now the list of locations that must
 * be passed to this procedure. The modification will be to either add
 * dummy locations to actuals, delete from actuals, or leave it
 * unchanged.
 * Add "dummy" params: this will be required when there are
 *     less live outs at a call site than the number of parameters
 *     expected by the procedure called. This will be a result of
 *     one of two things:
 *     i) a value returned by a preceeding call is used as a
 *        parameter and as such is not detected as defined by the
 *        procedure. E.g.:
 *
 *           foo(bar(x));
 *
 *        Here, the result of bar(x) is used as the first and only
 *        parameter to foo. On some architectures (such as SPARC),
 *        the location used as the first parameter (e.g. %o0) is
 *        also the location in which a value is returned. So, the
 *        call to bar defines this location implicitly as shown in
 *        the following SPARC assembly that may be generated by from
 *        the above code:
 *
 *            mov      x, %o0
 *            call  bar
 *            nop
 *            call  foo
 *
 *       As can be seen, there is no definition of %o0 after the
 *       call to bar and before the call to foo. Adding the integer
 *       return location is therefore a good guess for the dummy
 *       location to add (but may occasionally be wrong).
 *
 *    ii) uninitialised variables are used as parameters to a call
 *
 *    Note that both of these situations can only occur on
 *    architectures such as SPARC that use registers for parameter
 *    passing. Stack parameters must always be pushed so that the
 *    callee doesn't access the caller's non-parameter portion of
 *    stack.
 *
 * This used to be a virtual function, implemented differenty for
 * LibProcs and for UserProcs. But in fact, both need the exact same
 * treatment; the only difference is how the local member "parameters"
 * is set (from common.hs in the case of LibProc objects, or from analysis
 * in the case of UserProcs).
 */
void Proc::matchParams(std::list<Exp *> &actuals, UserProc &caller) {
    //TODO: not implemented, not used, but large amount of docs :)
}

/**
 * Get a list of types to cast a given list of actual parameters to
 */
std::list<Type> *Proc::getParamTypeList(const std::list<Exp *> &actuals) {
    //TODO: not implemented, not used
    return 0;
}

void UserProc::renameParam(const char *oldName, const char *newName) {
    oldName = strdup(oldName);
    Proc::renameParam(oldName, newName);
    //cfg->searchAndReplace(Location::param(strdup(oldName), this), Location::param(strdup(newName), this));
}

void UserProc::setParamType(const char* nam, Type* ty) {
    signature->setParamType(nam, ty);
}

void UserProc::setParamType(int idx, Type* ty) {
    int n = 0;
    StatementList::iterator it;
    for (it = parameters.begin(); n != idx && it != parameters.end(); it++, n++) // find n-th parameter it
        ;
    if (it != parameters.end()) {
        Assign *a = (Assign*)*it;
        a->setType(ty);
        // Sometimes the signature isn't up to date with the latest parameters
        signature->setParamType(a->getLeft(), ty);
    }
}

void UserProc::renameLocal(const char *oldName, const char *newName) {
    Type *ty = locals[oldName];
    const Exp *oldExp = expFromSymbol(oldName);
    locals.erase(oldName);
    Exp *oldLoc = getSymbolFor(oldExp, ty);
    Exp *newLoc = Location::local(strdup(newName), this);
    mapSymbolToRepl(oldExp, oldLoc, newLoc);
    locals[newName] = ty;
    cfg->searchAndReplace(oldLoc, newLoc);
}

bool UserProc::searchAll(Exp* search, std::list<Exp*> &result) {
    return cfg->searchAll(search, result);
}

void Proc::printCallGraphXML(std::ostream &os, int depth, bool recurse) {
    if (!DUMP_XML)
        return;
    visited = true;
    for (int i = 0; i < depth; i++)
        os << "      ";
    os << "<proc name=\"" << getName() << "\"/>\n";
}

void UserProc::printCallGraphXML(std::ostream &os, int depth, bool recurse) {
    if (!DUMP_XML)
        return;
    bool wasVisited = visited;
    visited = true;
    int i;
    for (i = 0; i < depth; i++)
        os << "      ";
    os << "<proc name=\"" << getName() << "\">\n";
    if (recurse) {
        for (std::list<Proc*>::iterator it = calleeList.begin(); it != calleeList.end(); it++)
            (*it)->printCallGraphXML(os, depth+1, !wasVisited && !(*it)->isVisited());
    }
    for (i = 0; i < depth; i++)
        os << "      ";
    os << "</proc>\n";
}

void Proc::printDetailsXML() {
    if (!DUMP_XML)
        return;
    std::ofstream out((Boomerang::get()->getOutputPath() + getName() + "-details.xml").c_str());
    out << "<proc name=\"" << getName() << "\">\n";
    unsigned i;
    for (i = 0; i < signature->getNumParams(); i++)
        out << "   <param name=\"" << signature->getParamName(i) << "\" " << "exp=\"" << signature->getParamExp(i)
            << "\" " << "type=\"" << signature->getParamType(i)->getCtype() << "\"\n";
    for (i = 0; i < signature->getNumReturns(); i++)
        out << "   <return exp=\"" << signature->getReturnExp(i) << "\" "
            << "type=\"" << signature->getReturnType(i)->getCtype() << "\"/>\n";
    out << "</proc>\n";
    out.close();
}

void UserProc::printDecodedXML() {
    if (!DUMP_XML)
        return;
    std::ofstream out((Boomerang::get()->getOutputPath() + getName() + "-decoded.xml").c_str());
    out << "<proc name=\"" << getName() << "\">\n";
    out << "    <decoded>\n";
    std::ostringstream os;
    print(os);
    std::string s = os.str();
    escapeXMLChars(s);
    out << s;
    out << "    </decoded>\n";
    out << "</proc>\n";
    out.close();
}

void UserProc::printAnalysedXML() {
    if (!DUMP_XML)
        return;
    std::ofstream out((Boomerang::get()->getOutputPath() + getName() + "-analysed.xml").c_str());
    out << "<proc name=\"" << getName() << "\">\n";
    out << "    <analysed>\n";
    std::ostringstream os;
    print(os);
    std::string s = os.str();
    escapeXMLChars(s);
    out << s;
    out << "    </analysed>\n";
    out << "</proc>\n";
    out.close();
}

void UserProc::printSSAXML() {
    if (!DUMP_XML)
        return;
    std::ofstream out((Boomerang::get()->getOutputPath() + getName() + "-ssa.xml").c_str());
    out << "<proc name=\"" << getName() << "\">\n";
    out << "    <ssa>\n";
    std::ostringstream os;
    print(os);
    std::string s = os.str();
    escapeXMLChars(s);
    out << s;
    out << "    </ssa>\n";
    out << "</proc>\n";
    out.close();
}


void UserProc::printXML() {
    if (!DUMP_XML)
        return;
    printDetailsXML();
    printSSAXML();
    prog->printCallGraphXML();
    printUseGraph();
}

void UserProc::printUseGraph() {
    std::ofstream out((Boomerang::get()->getOutputPath() + getName() + "-usegraph.dot").c_str());
    out << "digraph " << getName() << " {\n";
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        if (s->isPhi())
            out << s->getNumber() << " [shape=diamond];\n";
        LocationSet refs;
        s->addUsedLocs(refs);
        LocationSet::iterator rr;
        for (rr = refs.begin(); rr != refs.end(); rr++) {
            if (((Exp*)*rr)->isSubscript()) {
                RefExp *r = (RefExp*)*rr;
                if (r->getDef())
                    out << r->getDef()->getNumber() << " -> " << s->getNumber() << ";\n";
            }
        }
    }
    out << "}\n";
    out.close();
}

/***************************************************************************//**
 *
 * \brief        Output operator for a Proc object.
 * \param        os - output stream
 * \param        proc -
 * \returns      os
 ******************************************************************************/
//std::ostream& operator<<(std::ostream& os, Proc& proc) {
//    return proc.put(os);
//}

//! Get the first procedure that calls this procedure (or null for main/start).
Proc *Proc::getFirstCaller() {
    if (m_firstCaller == nullptr && m_firstCallerAddr != NO_ADDRESS) {
        m_firstCaller = prog->findProc(m_firstCallerAddr);
        m_firstCallerAddr = NO_ADDRESS;
    }
    return m_firstCaller;
}

/**********************
 * LibProc methods.
 *********************/

/***************************************************************************//**
 *
 * \brief        Constructor with name, native address.
 * \param        name - Name of procedure
 * \param        uNative - Native address of entry point of procedure
 ******************************************************************************/
LibProc::LibProc(Prog *prog, std::string& name, ADDRESS uNative) : Proc(prog, uNative, nullptr) {
    Signature* sig = prog->getLibSignature(name.c_str());
    signature = sig;
}

LibProc::~LibProc()
{}

/***************************************************************************//**
 *
 * \brief        Display on os.
 * PARAMETERS:        os -
 * \returns            os
 ******************************************************************************/
//std::ostream& LibProc::put(std::ostream& os) {
//    os << "library procedure `" << signature->getName() << "' resides at 0x";
//    return os << std::hex << address << std::endl;
//}

//! Get the RHS that is proven for left
Exp *LibProc::getProven(Exp *left) {
    // Just use the signature information (all we have, after all)
    return signature->getProven(left);
}

bool LibProc::isPreserved(Exp* e) {
    return signature->isPreserved(e);
}

/**********************
 * UserProc methods.
 *********************/

/***************************************************************************//**
 *
 * \brief        Constructor with name, native address.
 * \param name - Name of procedure
 * \param uNative - Native address of entry point of procedure
 *
 ******************************************************************************/
UserProc::UserProc() : Proc(), cfg(nullptr), status(PROC_UNDECODED),
    // decoded(false), analysed(false),
    nextLocal(0), nextParam(0),    // decompileSeen(false), decompiled(false), isRecursive(false)
    cycleGrp(nullptr), theReturnStatement(nullptr) {
    localTable.setProc(this);
}
UserProc::UserProc(Prog *prog, std::string& name, ADDRESS uNative) :
    // Not quite ready for the below fix:
    // Proc(prog, uNative, prog->getDefaultSignature(name.c_str())),
    Proc(prog, uNative, new Signature(name.c_str())),
    cfg(new Cfg()), status(PROC_UNDECODED),
    nextLocal(0),  nextParam(0),// decompileSeen(false), decompiled(false), isRecursive(false),
    cycleGrp(nullptr), theReturnStatement(nullptr), DFGcount(0)
{
    cfg->setProc(this);                 // Initialise cfg.myProc
    localTable.setProc(this);
}

UserProc::~UserProc() {
    if (cfg)
        delete cfg;
}

/***************************************************************************//**
 *
 * \brief        Deletes the whole Cfg for this proc object. Also clears the
 * cfg pointer, to prevent strange errors after this is called
 *
 ******************************************************************************/
void UserProc::deleteCFG() {
    delete cfg;
    cfg = nullptr;
}

class lessEvaluate : public std::binary_function<SyntaxNode*, SyntaxNode*, bool> {
public:
    bool operator()(const SyntaxNode* x, const SyntaxNode* y) const {
        return ((SyntaxNode*)x)->getScore() > ((SyntaxNode*)y)->getScore();
    }
};
/**
 * Returns an abstract syntax tree for the procedure in the internal representation. This function actually
 * _calculates_ * this value and is expected to do so expensively.
 */
SyntaxNode *UserProc::getAST() {
    int numBBs = 0;
    BlockSyntaxNode *init = new BlockSyntaxNode();
    BB_IT it;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
        BlockSyntaxNode *b = new BlockSyntaxNode();
        b->setBB(bb);
        init->addStatement(b);
        numBBs++;
    }

    // perform a best first search for the nicest AST
    std::priority_queue<SyntaxNode*, std::vector<SyntaxNode*>, lessEvaluate > ASTs;
    ASTs.push(init);

    SyntaxNode *best = init;
    int best_score = init->getScore();
    int count = 0;
    while (ASTs.size()) {
        if (best_score < numBBs * 2) {
            LOG << "exit early: " << best_score << "\n";
            break;
        }

        SyntaxNode *top = ASTs.top();
        ASTs.pop();
        int score = top->evaluate(top);

        printAST(top); // debug

        if (score < best_score) {
            if (best && top != best)
                delete best;
            best = top;
            best_score = score;
        }

        count++;
        if (count > 100)
            break;

        // add successors
        std::vector<SyntaxNode*> successors;
        top->addSuccessors(top, successors);
        for (unsigned i = 0; i < successors.size(); i++) {
            //successors[i]->addToScore(top->getScore());    // uncomment for A*
            successors[i]->addToScore(successors[i]->getDepth()); // or this
            ASTs.push(successors[i]);
        }

        if (top != best)
            delete top;
    }

    // clean up memory
    while(ASTs.size()) {
        SyntaxNode *top = ASTs.top();
        ASTs.pop();
        if (top != best)
            delete top;
    }

    return best;
}

int count = 1;
//! Print ast to a file
void UserProc::printAST(SyntaxNode *a) {
    char s[1024];
    if (a == nullptr)
        a = getAST();
    sprintf(s, "ast%i-%s.dot", count++, getName());
    std::ofstream of(s);
    of << "digraph " << getName() << " {" << std::endl;
    of << "     label=\"score: " << a->evaluate(a) << "\";" << std::endl;
    a->printAST(a, of);
    of << "}" << std::endl;
    of.close();
}

/***************************************************************************//**
 *
 * \brief Records that this procedure has been decoded.
 *
 ******************************************************************************/
void UserProc::setDecoded() {
    setStatus(PROC_DECODED);
    printDecodedXML();
}

/***************************************************************************//**
 *
 * \brief Removes the decoded bit and throws away all the current information
 * about this procedure.
 *
 ******************************************************************************/
void UserProc::unDecode() {
    cfg->clear();
    setStatus(PROC_UNDECODED);
}

/***************************************************************************//**
 *
 * \brief    Get the BB with the entry point address for this procedure
 * \note (not always the first BB)
 * \returns        Pointer to the entry point BB, or nullptr if not found
 *
 ******************************************************************************/
BasicBlock * UserProc::getEntryBB() {
    return cfg->getEntryBB();
}

/***************************************************************************//**
 *
 * \brief        Set the entry BB for this procedure (constructor has the entry address)
 *
 ******************************************************************************/
void UserProc::setEntryBB() {
    std::list<BasicBlock *>::iterator bbit;
    BasicBlock * pBB = cfg->getFirstBB(bbit);        // Get an iterator to the first BB
    // Usually, but not always, this will be the first BB, or at least in the first few
    while (pBB && address != pBB->getLowAddr()) {
        pBB = cfg->getNextBB(bbit);
    }
    cfg->setEntryBB(pBB);
}

/***************************************************************************//**
 *
 * \brief Add this callee to the set of callees for this proc
 * \param  callee - A pointer to the Proc object for the callee
 *
 ******************************************************************************/
void UserProc::addCallee(Proc* callee) {
    // is it already in? (this is much slower than using a set)
    std::list<Proc*>::iterator cc;
    for (cc = calleeList.begin(); cc != calleeList.end(); cc++)
        if(*cc == callee)
            return; // it's already in

    calleeList.push_back(callee);
}
/// code generation
void UserProc::generateCode(HLLCode *hll) {
    assert(cfg);
    assert(getEntryBB());

    cfg->structure();
    removeUnusedLocals();

    // Note: don't try to remove unused statements here; that requires the
    // RefExps, which are all gone now (transformed out of SSA form)!

    if (VERBOSE || Boomerang::get()->printRtl)
        LOG << *this;

    hll->AddProcStart(this);

    // Local variables; print everything in the locals map
    std::map<std::string, Type*>::iterator last = locals.end();
    if (locals.size()) last--;
    for (std::map<std::string, Type*>::iterator it = locals.begin(); it != locals.end(); it++) {
        Type* locType = it->second;
        if (locType == nullptr || locType->isVoid())
            locType = new IntegerType();
        hll->AddLocal(it->first.c_str(), locType, it == last);
    }

    if (Boomerang::get()->noDecompile && std::string(getName()) == "main") {
        StatementList args, results;
        if (prog->getFrontEndId() == PLAT_PENTIUM)
            hll->AddCallStatement(1, nullptr, "PENTIUMSETUP", args, &results);
        else if (prog->getFrontEndId() == PLAT_SPARC)
            hll->AddCallStatement(1, nullptr, "SPARCSETUP", args, &results);
    }

    std::list<BasicBlock *> followSet, gotoSet;
    getEntryBB()->generateCode(hll, 1, nullptr, followSet, gotoSet, this);

    hll->AddProcEnd();

    if (!Boomerang::get()->noRemoveLabels)
        cfg->removeUnneededLabels(hll);

    setStatus(PROC_CODE_GENERATED);
}

/// print this proc, mainly for debugging
void UserProc::print(std::ostream &out, bool html) const {
    signature->print(out, html);
    if (html)
        out << "<br>";
    out << "in cluster " << cluster->getName() << "\n";
    if (html)
        out << "<br>";
    std::ostringstream ost;
    printParams(ost, html);
    dumpLocals(ost, html);
    out << ost.str().c_str();
    printSymbolMap(out, html);
    if (html)
        out << "<br>";
    out << "live variables: ";
    std::ostringstream ost2;
    col.print(ost2);
    out << ost2.str().c_str() << "\n";
    if (html)
        out << "<br>";
    out << "end live variables\n";
    std::ostringstream ost3;
    cfg->print(ost3, html);
    out << ost3.str().c_str();
    out << "\n";
}

void UserProc::setStatus(ProcStatus s) {
    status = s;
    Boomerang::get()->alert_proc_status_change(this);
}

void UserProc::printParams( std::ostream &out, bool html /*= false*/ ) const {
    if (html)
        out << "<br>";
    out << "parameters: ";
    bool first = true;
    for (StatementList::const_iterator pp = parameters.begin(); pp != parameters.end(); ++pp) {
        if (first)
            first = false;
        else
            out << ", ";
        out << ((Assign*)*pp)->getType() << " " << ((Assign*)*pp)->getLeft();
    }
    out << "\n";
    if (html)
        out << "<br>";
    out << "end parameters\n";
}

char* UserProc::prints() {
    std::ostringstream ost;
    print(ost);
    strncpy(debug_buffer, ost.str().c_str(), DEBUG_BUFSIZE);
    debug_buffer[DEBUG_BUFSIZE-1] = '\0';
    return debug_buffer;
}

void UserProc::dump() {
    print(std::cerr);
}

void UserProc::printDFG() const {
    char fname[1024];
    sprintf(fname, "%s%s-%i-dfg.dot", Boomerang::get()->getOutputPath().c_str(), getName(), DFGcount);
    DFGcount++;
    if (VERBOSE)
        LOG << "outputing DFG to " << fname << "\n";
    std::ofstream out(fname);
    out << "digraph " << getName() << " {\n";
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement *s = *it;
        if (s->isPhi())
            out << s->getNumber() << " [shape=\"triangle\"];\n";
        if (s->isCall())
            out << s->getNumber() << " [shape=\"box\"];\n";
        if (s->isBranch())
            out << s->getNumber() << " [shape=\"diamond\"];\n";
        LocationSet refs;
        s->addUsedLocs(refs);
        LocationSet::iterator rr;
        for (rr = refs.begin(); rr != refs.end(); rr++) {
            RefExp* r = dynamic_cast<RefExp*>(*rr);
            if (r) {
                if (r->getDef())
                    out << r->getDef()->getNumber();
                else
                    out << "input";
                out << " -> ";
                if (s->isReturn())
                    out << "output";
                else
                    out << s->getNumber();
                out << ";\n";
            }
        }
    }
    out << "}\n";
    out.close();
}

/***************************************************************************//**
 *
 * \brief Initialise the statements, e.g. proc, bb pointers
 *
 ******************************************************************************/
void UserProc::initStatements() {
    BB_IT it;
    BasicBlock::rtlit rit; StatementList::iterator sit;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
        for (Statement* s = bb->getFirstStmt(rit, sit); s; s = bb->getNextStmt(rit, sit)) {
            s->setProc(this);
            s->setBB(bb);
            CallStatement* call = dynamic_cast<CallStatement*>(s);
            if (call) {
                call->setSigArguments();
                if (call->getDestProc() && call->getDestProc()->isNoReturn() && bb->getNumOutEdges() == 1) {
                    BasicBlock * out = bb->getOutEdge(0);
                    if (out != cfg->getExitBB() || cfg->getExitBB()->getNumInEdges() != 1) {
                        out->deleteInEdge(bb);
                        bb->getOutEdges().clear();
                    }
                }
            }
        }
    }
}

void UserProc::numberStatements() {
    BB_IT it;
    BasicBlock::rtlit rit; StatementList::iterator sit;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
        for (Statement* s = bb->getFirstStmt(rit, sit); s; s = bb->getNextStmt(rit, sit))
            if (!s->isImplicit() &&         // Don't renumber implicits (remain number 0)
                    s->getNumber() == 0)    // Don't renumber existing (or waste numbers)
                s->setNumber(++stmtNumber);
    }
}


// get all statements
// Get to a statement list, so they come out in a reasonable and consistent order
/// get all the statements
void UserProc::getStatements(StatementList &stmts) const {
    BBC_IT it;
    for (const BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it))
        bb->getStatements(stmts);

    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++)
        if ((*it)->getProc() == nullptr)
            (*it)->setProc(const_cast<UserProc *>(this));
}

/***************************************************************************//**
 *
 * \brief Remove a statement
 *
 * Remove a statement. This is somewhat inefficient - we have to search the whole BB for the statement.
 * Should use iterators or other context to find out how to erase "in place" (without having to linearly search)
 *
 ******************************************************************************/
void UserProc::removeStatement(Statement *stmt) {
    // remove anything proven about this statement
    for (std::map<Exp*, Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); ) {
        LocationSet refs;
        it->second->addUsedLocs(refs);
        it->first->addUsedLocs(refs);        // Could be say m[esp{99} - 4] on LHS and we are deleting stmt 99
        LocationSet::iterator rr;
        bool usesIt = false;
        for (rr = refs.begin(); rr != refs.end(); rr++) {
            Exp* r = *rr;
            if (r->isSubscript() && ((RefExp*)r)->getDef() == stmt) {
                usesIt = true;
                break;
            }
        }
        if (usesIt) {
            if (VERBOSE)
                LOG << "removing proven true exp " << it->first << " = " << it->second <<
                       " that uses statement being removed.\n";
            provenTrue.erase(it++);
            // it = provenTrue.begin();
            continue;
        }
        ++it;            // it is incremented with the erase, or here
    }

    // remove from BB/RTL
    BasicBlock * bb = stmt->getBB();            // Get our enclosing BB
    std::list<RTL*> *rtls = bb->getRTLs();
    for (RTL* rit : *rtls) {
        for (RTL::iterator it = rit->begin(); it != rit->end(); it++) {
            if (*it == stmt) {
                rit->erase(it);
                return;
            }
        }
    }
}

void UserProc::insertAssignAfter(Statement* s, Exp* left, Exp* right) {
    std::list<Statement*>::iterator it;
    std::list<Statement*>* stmts;
    if (s == nullptr) {
        // This means right is supposed to be a parameter. We can insert the assignment at the start of the entryBB
        BasicBlock * entryBB = cfg->getEntryBB();
        std::list<RTL*> *rtls = entryBB->getRTLs();
        assert(rtls->size());        // Entry BB should have at least 1 RTL
        stmts = rtls->front();
        it = stmts->begin();
    } else {
        // An ordinary definition; put the assignment at the end of s's BB
        BasicBlock * bb = s->getBB();         // Get the enclosing BB for s
        std::list<RTL*> *rtls = bb->getRTLs();
        assert(rtls->size());        // If s is defined here, there should be
        // at least 1 RTL
        stmts = rtls->back();
        it = stmts->end();            // Insert before the end
    }
    Assign* as = new Assign(left, right);
    as->setProc(this);
    stmts->insert(it, as);
    return;
}

/// Insert statement \a a after statement \a s.
/// \note this procedure is designed for the front end, where enclosing BBs are not set up yet.
/// So this is an inefficient linear search!
void UserProc::insertStatementAfter(Statement* s, Statement* a) {
    BB_IT bb;
    for (bb = cfg->begin(); bb != cfg->end(); bb++) {
        std::list<RTL*>::iterator rr;
        std::list<RTL*>* rtls = (*bb)->getRTLs();
        if (rtls == nullptr)
            continue;            // e.g. *bb is (as yet) invalid
        for (RTL *rr : *rtls) {
            std::list<Statement*>::iterator ss;
            for (ss = rr->begin(); ss != rr->end(); ss++) {
                if (*ss == s) {
                    ss++;        // This is the point to insert before
                    rr->insert(ss, a);
                    return;
                }
            }
        }
    }
    assert(false);            // Should have found this statement in this BB
}

/* Cycle detection logic:
 * *********************
 * cycleGrp is an initially nullptr pointer to a set of procedures, representing the procedures involved in the current
 * recursion group, if any. These procedures have to be analysed together as a group, after individual pre-group
 * analysis.
 * child is a set of procedures, cleared at the top of decompile(), representing the cycles associated with the
 * current procedure and all of its children. If this is empty, the current procedure is not involved in recursion,
 * and can be decompiled up to and including removing unused statements.
 * path is an initially empty list of procedures, representing the call path from the current entry point to the
 * current procedure, inclusive.
 * If (after all children have been processed: important!) the first element in path and also cycleGrp is the current
 * procedure, we have the maximal set of distinct cycles, so we can do the recursion group analysis and return an empty
 * set. At the end of the recursion group analysis, the whole group is complete, ready for the global analyses.
 cycleSet decompile(ProcList path)        // path initially empty
        child = new ProcSet
        append this proc to path
        for each child c called by this proc
                if c has already been visited but not finished
                        // have new cycle
                        if c is in path
                          // this is a completely new cycle
                          insert every proc from c to the end of path into child
                        else
                          // this is a new branch of an existing cycle
                          child = c->cycleGrp
                          find first element f of path that is in cycleGrp
                          insert every proc after f to the end of path into child
                        for each element e of child
              insert e->cycleGrp into child
                          e->cycleGrp = child
                else
                        // no new cycle
                        tmp = c->decompile(path)
                        child = union(child, tmp)
                        set return statement in call to that of c
        if (child empty)
                earlyDecompile()
                child = middleDecompile()
                removeUnusedStatments()            // Not involved in recursion
        else
                // Is involved in recursion
                find first element f in path that is also in cycleGrp
                if (f == this)                        // The big test: have we got the complete strongly connected component?
                        recursionGroupAnalysis()        // Yes, we have
                        child = new ProcSet            // Don't add these processed cycles to the parent
        remove last element (= this) from path
        return child
 */
/***************************************************************************//**
 *
 * \brief Begin the decompile process at this procedure
 * \param  path - is a list of pointers to procedures, representing the path from
 * the current entry point to the current procedure in the call graph. Pass an
 * empty set at the top level.
 * \param indent is the indentation level; pass 0 at the top level
 *
 ******************************************************************************/
ProcSet* UserProc::decompile(ProcList* path, int& indent) {
    Boomerang::get()->alert_considering(path->empty() ? nullptr : path->back(), this);
    std::cout << std::setw(++indent) << " " << (status >= PROC_VISITED ? "re" : "") << "considering " << getName() <<
                 "\n";
    if (VERBOSE)
        LOG << "begin decompile(" << getName() << ")\n";

    // Prevent infinite loops when there are cycles in the call graph (should never happen now)
    if (status >= PROC_FINAL) {
        std::cerr << "Error: " << getName() << " already has status PROC_FINAL\n";
        return nullptr;                            // Already decompiled
    }
    if (status < PROC_DECODED)
        // Can happen e.g. if a callee is visible only after analysing a switch statement
        prog->reDecode(this);                    // Actually decoding for the first time, not REdecoding

    if (status < PROC_VISITED)
        setStatus(PROC_VISITED);                     // We have at least visited this proc "on the way down"
    ProcSet* child = new ProcSet;
    path->push_back(this);                        // Append this proc to path

    /*    *    *    *    *    *    *    *    *    *    *    *
         *                                            *
         *    R e c u r s e   t o   c h i l d r e n    *
         *                                            *
         *    *    *    *    *    *    *    *    *    *    *    */

    if (!Boomerang::get()->noDecodeChildren) {
        // Recurse to children first, to perform a depth first search
        BB_IT it;
        // Look at each call, to do the DFS
        for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
            if (bb->getType() == CALL) {
                // The call Statement will be in the last RTL in this BB
                CallStatement* call = (CallStatement*)bb->getRTLs()->back()->getHlStmt();
                if (!call->isCall()) {
                    LOG << "bb at " << bb->getLowAddr() << " is a CALL but last stmt is not a call: " << call << "\n";
                }
                assert(call->isCall());
                UserProc* c = (UserProc*)call->getDestProc();
                if (c == nullptr || c->isLib()) continue;
                if (c->status == PROC_FINAL) {
                    // Already decompiled, but the return statement still needs to be set for this call
                    call->setCalleeReturn(c->getTheReturnStatement());
                    continue;
                }
                // if c has already been visited but not done (apart from global analyses, i.e. we have a new cycle)
                if (c->status >= PROC_VISITED && c->status <= PROC_EARLYDONE) {
                    // if c is in path
                    ProcList::iterator pi;
                    bool inPath = false;
                    for (pi = path->begin(); pi != path->end(); ++pi) {
                        if (*pi == c) {
                            inPath = true;
                            break;
                        }
                    }
                    if (inPath) {
                        // This is a completely new cycle
                        // Insert every proc from c to the end of path into child
                        do {
                            child->insert(*pi);
                            ++pi;
                        } while (pi != path->end());
                    } else {
                        // This is new branch of an existing cycle
                        child = c->cycleGrp;
                        // Find first element f of path that is in c->cycleGrp
                        ProcList::iterator pi;
                        Proc* f = nullptr;
                        for (pi = path->begin(); pi != path->end(); ++pi) {
                            if (c->cycleGrp->find(*pi) != c->cycleGrp->end()) {
                                f = *pi;
                                break;
                            }
                        }
                        assert(f);
                        // Insert every proc after f to the end of path into child
                        // There must be at least one element in the list (this proc), so the ++pi should be safe
                        while (++pi != path->end()) {
                            child->insert(*pi);
                        }
                    }
                    // point cycleGrp for each element of child to child, unioning in each element's cycleGrp
                    ProcSet::iterator cc;
                    for (cc = child->begin(); cc != child->end(); ++cc) {
                        ProcSet*& cg = (*cc)->cycleGrp;
                        if (cg)
                            child->insert(cg->begin(), cg->end());
                        cg = child;
                    }
                    setStatus(PROC_INCYCLE);
                } else {
                    // No new cycle
                    if (VERBOSE)
                        LOG << "visiting on the way down child " << c->getName() << " from " << getName() << "\n";
                    ProcSet* tmp = c->decompile(path, indent);
                    child->insert(tmp->begin(), tmp->end());
                    // Child has at least done middleDecompile(), possibly more
                    call->setCalleeReturn(c->getTheReturnStatement());
                    if (tmp->size() > 0) {
                        setStatus(PROC_INCYCLE);
                    }
                }
            }
        }
    }


    // if child is empty, i.e. no child involved in recursion
    if (child->size() == 0) {
        Boomerang::get()->alert_decompiling(this);
        std::cout << std::setw(indent) << " " << "decompiling " << getName() << "\n";
        initialiseDecompile();                    // Sort the CFG, number statements, etc
        earlyDecompile();
        child = middleDecompile(path, indent);
        // If there is a switch statement, middleDecompile could contribute some cycles. If so, we need to test for
        // the recursion logic again
        if (child->size() != 0)
            // We've just come back out of decompile(), so we've lost the current proc from the path.
            path->push_back(this);
    }
    if (child->size() == 0) {
        remUnusedStmtEtc();    // Do the whole works
        setStatus(PROC_FINAL);
        Boomerang::get()->alert_end_decompile(this);
    } else {
        // this proc's children, and hence this proc, is/are involved in recursion
        // find first element f in path that is also in cycleGrp
        ProcList::iterator f;
        for (f = path->begin(); f != path->end(); ++f)
            if (cycleGrp->find(*f) != cycleGrp->end())
                break;
        // The big test: have we found all the strongly connected components (in the call graph)?
        if (*f == this) {
            // Yes, process these procs as a group
            recursionGroupAnalysis(path, indent);// Includes remUnusedStmtEtc on all procs in cycleGrp
            setStatus(PROC_FINAL);
            Boomerang::get()->alert_end_decompile(this);
            child = new ProcSet;
        }
    }

    // Remove last element (= this) from path
    // The if should not be neccesary, but nestedswitch needs it
    if (path->size())
        path->erase(--path->end());
    else
        LOG << "WARNING: UserProc::decompile: empty path when trying to remove last proc\n";

    --indent;
    if (VERBOSE)
        LOG << "end decompile(" << getName() << ")\n";
    return child;
}

/*    *    *    *    *    *    *    *    *    *    *    *
 *                                            *
 *        D e c o m p i l e   p r o p e r        *
 *            ( i n i t i a l )                *
 *                                            *
 *    *    *    *    *    *    *    *    *    *    *    */

/***************************************************************************//**
 *
 * \brief Initialise decompile: sort CFG, number statements, dominator tree, etc.
 *
 ******************************************************************************/
void UserProc::initialiseDecompile() {

    Boomerang::get()->alert_start_decompile(this);

    Boomerang::get()->alert_decompile_debug_point(this, "before initialise");

    if (VERBOSE) LOG << "initialise decompile for " << getName() << "\n";

    // Sort by address, so printouts make sense
    cfg->sortByAddress();

    // Initialise statements
    initStatements();

    if (VERBOSE) {
        LOG << "--- debug print before SSA for " << getName() << " ---\n"
            << *this
            << "=== end debug print before SSA for " << getName() << " ===\n\n";
    }

    // Compute dominance frontier
    df.dominators(cfg);

    // Number the statements
    stmtNumber = 0;
    numberStatements();

    printXML();

    if (Boomerang::get()->noDecompile) {
        std::cout << "not decompiling.\n";
        setStatus(PROC_FINAL);                // ??!
        return;
    }

    if (VERBOSE) {
        LOG << "--- debug initial print after decoding for " << getName() << " ---\n"
            << *this
            << "=== end initial debug print after decoding for " << getName() << " ===\n\n";
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after initialise");
}
/***************************************************************************//**
 *
 * \brief Early decompile: Place phi functions, number statements, first rename,
 * propagation: ready for preserveds.
 *
 ******************************************************************************/
void UserProc::earlyDecompile() {

    if (status >= PROC_EARLYDONE)
        return;

    Boomerang::get()->alert_decompile_debug_point(this, "before early");
    if (VERBOSE) LOG << "early decompile for " << getName() << "\n";

    // Update the defines in the calls. Will redo if involved in recursion
    updateCallDefines();

    // This is useful for obj-c
    replaceSimpleGlobalConstants();

    // First placement of phi functions, renaming, and initial propagation. This is mostly for the stack pointer
    //maxDepth = findMaxDepth() + 1;
    //if (Boomerang::get()->maxMemDepth < maxDepth)
    //    maxDepth = Boomerang::get()->maxMemDepth;
    // TODO: Check if this makes sense. It seems to me that we only want to do one pass of propagation here, since
    // the status == check had been knobbled below. Hopefully, one call to placing phi functions etc will be
    // equivalent to depth 0 in the old scheme
    if (VERBOSE)
        LOG << "placing phi functions 1st pass\n";
    // Place the phi functions
    df.placePhiFunctions(this);

    if (VERBOSE)
        LOG << "numbering phi statements 1st pass\n";
    numberStatements();                // Number them

    if (VERBOSE)
        LOG << "renaming block variables 1st pass\n";
    // Rename variables
    doRenameBlockVars(1, true);
    if (VERBOSE) {
        LOG << "\n--- after rename (1) for " << getName() << " 1st pass\n"
            << *this
            << "\n=== done after rename (1) for " << getName() << " 1st pass\n\n";
    }

    bool convert;
    propagateStatements(convert, 1);
    if (VERBOSE) {
        LOG << "\n--- after propagation (1) for " << getName() << " 1st pass ---\n"
            << *this
            << "\n=== done after propagation (1) for " << getName() << " 1st pass ===\n\n";
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after early");
}
/***************************************************************************//**
 *
 * \brief Middle decompile: All the decompilation from preservation up to
 * but not including removing unused statements.
 * \returns the cycle set from the recursive call to decompile()
 *
 ******************************************************************************/
ProcSet* UserProc::middleDecompile(ProcList* path, int indent) {

    Boomerang::get()->alert_decompile_debug_point(this, "before middle");

    // The call bypass logic should be staged as well. For example, consider m[r1{11}]{11} where 11 is a call.
    // The first stage bypass yields m[r1{2}]{11}, which needs another round of propagation to yield m[r1{-}-32]{11}
    // (which can safely be processed at depth 1).
    // Except that this is now inherent in the visitor nature of the latest algorithm.
    fixCallAndPhiRefs();            // Bypass children that are finalised (if any)
    bool convert;
    if (status != PROC_INCYCLE)        // FIXME: need this test?
        propagateStatements(convert, 2);
    if (VERBOSE) {
        LOG << "\n--- after call and phi bypass (1) of " << getName() << " ---\n"
            << *this
            << "\n=== done after call and phi bypass (1) of " << getName() << " ===\n\n";
    }

    // This part used to be calle middleDecompile():

    findSpPreservation();
    // Oops - the idea of splitting the sp from the rest of the preservations was to allow correct naming of locals
    // so you are alias conservative. But of course some locals are ebp (etc) based, and so these will never be correct
    // until all the registers have preservation analysis done. So I may as well do them all together here.
    findPreserveds();
    fixCallAndPhiRefs();     // Propagate and bypass sp
    if (VERBOSE) {
        LOG << "--- after preservation, bypass and propagation ---\n"
            << *this
            << "=== end after preservation, bypass and propagation ===\n";
    }
    // Oh, no, we keep doing preservations till almost the end...
    //setStatus(PROC_PRESERVEDS);        // Preservation done

    if (!Boomerang::get()->noPromote)
        // We want functions other than main to be promoted. Needed before mapExpressionsToLocals
        promoteSignature();
    // The problem with doing locals too early is that the symbol map ends up with some {-} and some {0}
    // Also, once named as a local, it is tempting to propagate the memory location, but that might be unsafe if the
    // address is taken. But see mapLocalsAndParams just a page below.
    //mapExpressionsToLocals();

    // Update the arguments for calls (mainly for the non recursion affected calls)
    // We have only done limited propagation and collecting to this point. Need e.g. to put m[esp-K]
    // into the collectors of calls, so when a stack parameter is created, it will be correctly localised
    // Note that we'd like to limit propagation before this point, because we have not yet created any arguments, so
    // it is possible to get "excessive propagation" to parameters. In fact, because uses vary so much throughout a
    // program, it may end up better not limiting propagation until very late in the decompilation, and undoing some
    // propagation just before removing unused statements. Or even later, if that is possible.
    // For now, we create the initial arguments here (relatively early), and live with the fact that some apparently
    // distinct memof argument expressions (e.g. m[eax{30}] and m[esp{40}-4]) will turn out to be duplicates, and so
    // the duplicates must be eliminated.
    bool change = df.placePhiFunctions(this);
    if (change) numberStatements();        // Number the new statements
    doRenameBlockVars(2);
    propagateStatements(convert, 2);    // Otherwise sometimes sp is not fully propagated
    // Map locals and temporary parameters as symbols, so that they can be propagated later
    //    mapLocalsAndParams();                // FIXME: unsure where this belongs
    updateArguments();
    reverseStrengthReduction();
    //processTypes();

    // Repeat until no change
    int pass;
    for (pass = 3; pass <= 12; ++pass) {
        // Redo the renaming process to take into account the arguments
        if (VERBOSE)
            LOG << "renaming block variables (2) pass " << pass << "\n";
        // Rename variables
        change = df.placePhiFunctions(this);
        if (change) numberStatements();        // Number the new statements
        change |= doRenameBlockVars(pass, false);        // E.g. for new arguments

        // Seed the return statement with reaching definitions
        // FIXME: does this have to be in this loop?
        if (theReturnStatement) {
            theReturnStatement->updateModifieds();        // Everything including new arguments reaching the exit
            theReturnStatement->updateReturns();
        }

        printXML();

        // Print if requested
        if (VERBOSE) {        // was if debugPrintSSA
            LOG << "--- debug print SSA for " << getName() << " pass " << pass << " (no propagations) ---\n"
                << *this
                << "=== end debug print SSA for " << getName() << " pass " << pass << " (no propagations) ===\n\n";
        }

        if (!Boomerang::get()->dotFile.empty())             // Require -gd now (though doesn't listen to file name)
            printDFG();
        Boomerang::get()->alert_decompile_SSADepth(this, pass);    // FIXME: need depth -> pass in GUI code

        // (* Was: mapping expressions to Parameters as we go *)

#if 1    // FIXME: Check if this is needed any more. At least fib seems to need it at present.
        if (!Boomerang::get()->noChangeSignatures) {
            // addNewReturns(depth);
            for (int i=0; i < 3; i++) {        // FIXME: should be iterate until no change
                if (VERBOSE)
                    LOG << "### update returns loop iteration " << i << " ###\n";
                if (status != PROC_INCYCLE)
                    doRenameBlockVars(pass, true);
                findPreserveds();
                updateCallDefines();        // Returns have uses which affect call defines (if childless)
                fixCallAndPhiRefs();
                findPreserveds();            // Preserveds subtract from returns
            }
            printXML();
            if (VERBOSE) {
                LOG << "--- debug print SSA for " << getName() << " at pass " << pass <<
                       " (after updating returns) ---\n"
                    << *this
                    << "=== end debug print SSA for " << getName() << " at pass " << pass << " ===\n\n";
            }
        }
#endif

        printXML();
        // Print if requested
        if (VERBOSE) {        // was if debugPrintSSA
            LOG << "--- debug print SSA for " << getName() << " at pass "<< pass <<" (after trimming return set) ---\n"
                << *this
                << "=== end debug print SSA for " << getName() << " at pass " << pass << " ===\n\n";
        }

        Boomerang::get()->alert_decompile_beforePropagate(this, pass);
        Boomerang::get()->alert_decompile_debug_point(this, "before propagating statements");

        // Propagate
        bool convert;            // True when indirect call converted to direct
        do {
            convert = false;
            if (VERBOSE)
                LOG << "propagating at pass " << pass << "\n";
            change |= propagateStatements(convert, pass);
            change |= doRenameBlockVars(pass, true);
            // If you have an indirect to direct call conversion, some propagations that were blocked by
            // the indirect call might now succeed, and may be needed to prevent alias problems
            // FIXME: I think that the below, and even the convert parameter to propagateStatements(), is no longer
            // needed - MVE
            if (convert) {
                if (VERBOSE)
                    LOG << "\nabout to restart propagations and dataflow at pass " << pass <<
                           " due to conversion of indirect to direct call(s)\n\n";
                df.setRenameLocalsParams(false);
                change |= doRenameBlockVars(0, true);             // Initial dataflow level 0
                LOG << "\nafter rename (2) of " << getName() << ":\n"
                    << *this
                    << "\ndone after rename (2) of " << getName() << ":\n\n";
            }
        } while (convert);

        printXML();
        if (VERBOSE) {
            LOG << "--- after propagate for " << getName() << " at pass " << pass << " ---\n"
                << *this
                << "=== end propagate for " << getName() << " at pass " << pass << " ===\n\n";
        }

        Boomerang::get()->alert_decompile_afterPropagate(this, pass);
        Boomerang::get()->alert_decompile_debug_point(this, "after propagating statements");

        // this is just to make it readable, do NOT rely on these statements being removed
        removeSpAssignsIfPossible();
        // The problem with removing %flags and %CF is that %CF is a subset of %flags
        //removeMatchingAssignsIfPossible(new Terminal(opFlags));
        //removeMatchingAssignsIfPossible(new Terminal(opCF));
        removeMatchingAssignsIfPossible(new Unary(opTemp, new Terminal(opWildStrConst)));
        removeMatchingAssignsIfPossible(new Terminal(opPC));

        //processTypes();

        if (!change)
            break;                // Until no change
    }

    // At this point, there will be some memofs that have still not been renamed. They have been prevented from
    // getting renamed so that they didn't get renamed incorrectly (usually as {-}), when propagation and/or bypassing
    // may have ended up changing the address expression. There is now no chance that this will happen, so we need
    // to rename the existing memofs. Note that this can still link uses to definitions, e.g.
    // 50 r26 := phi(...)
    // 51 m[r26{50}] := 99;
    //    ... := m[r26{50}]{should be 51}

    if (VERBOSE)
        LOG << "### allowing SSA renaming of all memof expressions ###\n";
    df.setRenameLocalsParams(true);

    // Now we need another pass to inert phis for the memofs, rename them and propagate them
    ++pass;
    if (VERBOSE)
        LOG << "setting phis, renaming block variables after memofs renamable pass " << pass << "\n";
    change = df.placePhiFunctions(this);
    if (change) numberStatements();        // Number the new statements
    doRenameBlockVars(pass, false);        // MVE: do we want this parameter false or not?
    if (VERBOSE) {
        LOG << "--- after setting phis for memofs, renaming them for " << getName() << "\n"
            << *this
            << "=== done after setting phis for memofs, renaming them for " << getName() << "\n";
    }
    propagateStatements(convert, pass);
    // Now that memofs are renamed, the bypassing for memofs can work
    fixCallAndPhiRefs();            // Bypass children that are finalised (if any)

#if 0            // Now also done where ellipsis processing is done for dfa-based TA
    // Note: processConstants is also where ellipsis processing is done
    if (processConstants()) {
        if (status != PROC_INCYCLE) {
            doRenameBlockVars(-1, true);            // Needed if there was an indirect call to an ellipsis function
        }
    }
    processTypes();
#endif

    if (!Boomerang::get()->noParameterNames) {
        // ? Crazy time to do this... haven't even done "final" parameters as yet
        //mapExpressionsToParameters();
    }

    // Check for indirect jumps or calls not already removed by propagation of constants
    if (cfg->decodeIndirectJmp(this)) {
        // There was at least one indirect jump or call found and decoded. That means that most of what has been done
        // to this function so far is invalid. So redo everything. Very expensive!!
        // Code pointed to by the switch table entries has merely had FrontEnd::processFragment() called on it
        LOG << "=== about to restart decompilation of " << getName() <<
               " because indirect jumps or calls have been analysed\n\n";
        Boomerang::get()->alert_decompile_debug_point(this, "before restarting decompilation because indirect jumps or calls have been analysed");

        // First copy any new indirect jumps or calls that were decoded this time around. Just copy them all, the map
        // will prevent duplicates
        processDecodedICTs();
        // Now, decode from scratch
        theReturnStatement = nullptr;
        cfg->clear();
        std::ofstream os;
        prog->reDecode(this);
        df.setRenameLocalsParams(false);        // Start again with memofs
        setStatus(PROC_VISITED);                // Back to only visited progress
        path->erase(--path->end());                // Remove self from path
        --indent;                                // Because this is not recursion
        ProcSet* ret = decompile(path, indent);    // Restart decompiling this proc
        ++indent;                                // Restore indent
        path->push_back(this);                    // Restore self to path
        // It is important to keep the result of this call for the recursion analysis
        return ret;
    }

    findPreserveds();


    // Used to be later...
    if (!Boomerang::get()->noParameterNames) {
        //findPreserveds();        // FIXME: is this necessary here?
        //fixCallBypass();    // FIXME: surely this is not necessary now?
        //trimParameters();    // FIXME: surely there aren't any parameters to trim yet?
        if (VERBOSE) {
            LOG << "--- after replacing expressions, trimming params and returns for " << getName() << " ---\n"
                << *this
                << "=== end after replacing expressions, trimming params and returns for " << getName() << " ===\n";
        }
    }

    eliminateDuplicateArgs();

    if (VERBOSE)
        LOG << "===== end early decompile for " << getName() << " =====\n\n";
    setStatus(PROC_EARLYDONE);

    Boomerang::get()->alert_decompile_debug_point(this, "after middle");

    return new ProcSet;
}

/*    *    *    *    *    *    *    *    *    *    *    *    *    *
 *                                                    *
 *    R e m o v e   u n u s e d   s t a t e m e n t s    *
 *                                                    *
 *    *    *    *    *    *    *    *    *    *    *    *    *    */
//! Remove unused statements.
void UserProc::remUnusedStmtEtc() {

    bool convert;
    bool change;
    // NO! Removing of unused statements is an important part of the global removing unused returns analysis, which
    // happens after UserProc::decompile is complete
    //if (status >= PROC_FINAL)
    //    return;

    Boomerang::get()->alert_decompiling(this);
    Boomerang::get()->alert_decompile_debug_point(this, "before final");

    if (VERBOSE)
        LOG << "--- remove unused statements for " << getName() << " ---\n";
    // A temporary hack to remove %CF = %CF{7} when 7 isn't a SUBFLAGS
    //    if (theReturnStatement)
    //        theReturnStatement->specialProcessing();

    // Perform type analysis. If we are relying (as we are at present) on TA to perform ellipsis processing,
    // do the local TA pass now. Ellipsis processing often reveals additional uses (e.g. additional parameters
    // to printf/scanf), and removing unused statements is unsafe without full use information
    if (status < PROC_FINAL) {
        typeAnalysis();
        // Now that locals are identified, redo the dataflow
        change = df.placePhiFunctions(this);
        if (change)
            numberStatements();        // Number the new statements
        doRenameBlockVars(20);                // Rename the locals
        propagateStatements(convert, 20);    // Surely need propagation too
        if (VERBOSE) {
            LOG << "--- after propagating locals for " << getName() << " ---\n"
                << *this
                << "=== end after propagating locals for " << getName() << " ===\n\n";
        }
#if 0
        // Note: processConstants is also where ellipsis processing is done
        if (processConstants()) {
            if (status != PROC_INCYCLE) {
                doRenameBlockVars(-1, true);            // Needed if there was an indirect call to an ellipsis function
            }
        }
#endif

    }

    // Only remove unused statements after decompiling as much as possible of the proc
    // Remove unused statements
    RefCounter refCounts;            // The map
    // Count the references first
    countRefs(refCounts);
    // Now remove any that have no used
    if (!Boomerang::get()->noRemoveNull)
        remUnusedStmtEtc(refCounts);

    // Remove null statements
    if (!Boomerang::get()->noRemoveNull)
        removeNullStatements();

    printXML();
    if (VERBOSE && !Boomerang::get()->noRemoveNull) {
        LOG << "--- after removing unused and null statements pass " << 1 << " for " << getName() << " ---\n"
            << *this
            << "=== end after removing unused statements for " << getName() << " ===\n\n";
    }
    Boomerang::get()->alert_decompile_afterRemoveStmts(this, 1);

    findFinalParameters();
    if (!Boomerang::get()->noParameterNames) {
        // Replace the existing temporary parameters with the final ones:
        //mapExpressionsToParameters();
        addParameterSymbols();

        if (VERBOSE) {
            LOG << "--- after adding new parameters ---\n"
                << *this
                << "=== end after adding new parameters ===\n";
        }
    }

#if 0                // Construction zone; pay no attention
    bool convert;
    propagateStatements(convert, 222);    // This is the first opportunity to safely propagate memory parameters
    if (VERBOSE) {
        LOG << "--- after propagating new parameters ---\n";
        printToLog();
        LOG << "=== end after propagating new parameters ===\n";
    }
#endif

    updateCalls();                // Or just updateArguments?

    branchAnalysis();
    fixUglyBranches();

    if (VERBOSE) {
        LOG << "--- after remove unused statements etc for " << getName() << "\n"
            << *this
            << "=== after remove unused statements etc for " << getName() << "\n";
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after final");
}

void UserProc::remUnusedStmtEtc(RefCounter& refCounts) {

    Boomerang::get()->alert_decompile_debug_point(this, "before remUnusedStmtEtc");

    StatementList stmts;
    getStatements(stmts);
    bool change;
    do {                                // FIXME: check if this is ever needed
        change = false;
        StatementList::iterator ll = stmts.begin();
        while (ll != stmts.end()) {
            Statement* s = *ll;
            if (!s->isAssignment()) {
                // Never delete a statement other than an assignment (e.g. nothing "uses" a Jcond)
                ll++;
                continue;
            }
            Assignment* as = (Assignment*)s;
            Exp* asLeft = as->getLeft();
            // If depth < 0, consider all depths
            //if (asLeft && depth >= 0 && asLeft->getMemDepth() > depth) {
            //    ll++;
            //    continue;
            //}
            if (asLeft && asLeft->getOper() == opGlobal) {
                // assignments to globals must always be kept
                ll++;
                continue;
            }
            // If it's a memof and renameable it can still be deleted
            if (asLeft->getOper() == opMemOf && !canRename(asLeft)) {
                // Assignments to memof-anything-but-local must always be kept.
                ll++;
                continue;
            }
            if (asLeft->getOper() == opMemberAccess || asLeft->getOper() == opArrayIndex) {
                // can't say with these; conservatively never remove them
                ll++;
                continue;
            }
            if (refCounts.find(s) == refCounts.end() || refCounts[s] == 0) {    // Care not to insert unnecessarily
                // First adjust the counts, due to statements only referenced by statements that are themselves unused.
                // Need to be careful not to count two refs to the same def as two; refCounts is a count of the number
                // of statements that use a definition, not the total number of refs
                StatementSet stmtsRefdByUnused;
                LocationSet components;
                s->addUsedLocs(components, false);        // Second parameter false to ignore uses in collectors
                LocationSet::iterator cc;
                for (cc = components.begin(); cc != components.end(); cc++) {
                    if ((*cc)->isSubscript()) {
                        stmtsRefdByUnused.insert(((RefExp*)*cc)->getDef());
                    }
                }
                StatementSet::iterator dd;
                for (dd = stmtsRefdByUnused.begin(); dd != stmtsRefdByUnused.end(); dd++) {
                    if (*dd == nullptr) continue;
                    if (DEBUG_UNUSED)
                        LOG << "decrementing ref count of " << (*dd)->getNumber() << " because " << s->getNumber() <<
                               " is unused\n";
                    refCounts[*dd]--;
                }
                if (DEBUG_UNUSED)
                    LOG << "removing unused statement " << s->getNumber() << " " << s << "\n";
                removeStatement(s);
                ll = stmts.erase(ll);    // So we don't try to re-remove it
                change = true;
                continue;                // Don't call getNext this time
            }
            ll++;
        }
    } while (change);
    // Recaluclate at least the livenesses. Example: first call to printf in test/pentium/fromssa2, eax used only in a
    // removed statement, so liveness in the call needs to be removed
    removeCallLiveness();        // Kill all existing livenesses
    doRenameBlockVars(-2);        // Recalculate new livenesses
    setStatus(PROC_FINAL);        // Now fully decompiled (apart from one final pass, and transforming out of SSA form)

    Boomerang::get()->alert_decompile_debug_point(this, "after remUnusedStmtEtc");
}
/// Analyse the whole group of procedures for conditional preserveds, and update till no change.
/// Also finalise the whole group.
/***************************************************************************//**
 *
 * \brief Middle decompile: All the decompilation from preservation up to
 * but not including removing unused statements.
 * Returns the cycle set from the recursive call to decompile()
 *
 ******************************************************************************/
void UserProc::recursionGroupAnalysis(ProcList* path, int indent) {
    /* Overall algorithm:
        for each proc in the group
                initialise
                earlyDecompile
        for eac proc in the group
                middleDecompile
        mark all calls involved in cs as non-childless
        for each proc in cs
                update parameters and returns, redoing call bypass, until no change
        for each proc in cs
                remove unused statements
        for each proc in cs
                update parameters and returns, redoing call bypass, until no change
     */
    if (VERBOSE) {
        LOG << "\n\n# # # recursion group analysis for ";
        ProcSet::iterator csi;
        for (csi = cycleGrp->begin(); csi != cycleGrp->end(); ++csi)
            LOG << (*csi)->getName() << ", ";
        LOG << "# # #\n";
    }

    // First, do the initial decompile, and call earlyDecompile
    ProcSet::iterator curp;
    for (curp = cycleGrp->begin(); curp != cycleGrp->end(); ++curp) {
        (*curp)->setStatus(PROC_INCYCLE);                // So the calls are treated as childless
        Boomerang::get()->alert_decompiling(*curp);
        (*curp)->initialiseDecompile();                    // Sort the CFG, number statements, etc
        (*curp)->earlyDecompile();
    }

    // Now all the procs in the group should be ready for preservation analysis
    // The standard preservation analysis should automatically perform conditional preservation
    for (curp = cycleGrp->begin(); curp != cycleGrp->end(); ++curp) {
        (*curp)->middleDecompile(path, indent);
        (*curp)->setStatus(PROC_PRESERVEDS);
    }


    // FIXME: why exactly do we do this?
    // Mark all the relevant calls as non childless (will harmlessly get done again later)
    ProcSet::iterator it;
    for (it = cycleGrp->begin(); it != cycleGrp->end(); it++)
        (*it)->markAsNonChildless(cycleGrp);

    ProcSet::iterator p;
    // Need to propagate into the initial arguments, since arguments are uses, and we are about to remove unused
    // statements.
    bool convert;
    for (p = cycleGrp->begin(); p != cycleGrp->end(); ++p) {
        //(*p)->initialParameters();                    // FIXME: I think this needs to be mapping locals and params now
        (*p)->mapLocalsAndParams();
        (*p)->updateArguments();
        (*p)->propagateStatements(convert, 0);        // Need to propagate into arguments
    }

    // while no change
    for (int i=0; i < 2; i++) {
        for (p = cycleGrp->begin(); p != cycleGrp->end(); ++p) {
            (*p)->remUnusedStmtEtc();                // Also does final parameters and arguments at present
        }
    }
    if (VERBOSE)
        LOG << "=== end recursion group analysis ===\n";
    Boomerang::get()->alert_end_decompile(this);

}

/***************************************************************************//**
 *
 * \brief Update the defines and arguments in calls.
 *
 ******************************************************************************/
void UserProc::updateCalls() {
    if (VERBOSE)
        LOG << "### updateCalls for " << getName() << " ###\n";
    updateCallDefines();
    updateArguments();
    if (VERBOSE) {
        LOG << "--- after update calls for " << getName() << "\n"
            << *this
            << "=== after update calls for " << getName() << "\n";
    }
}

/***************************************************************************//**
 *
 * \brief Look for short circuit branching
 *
 ******************************************************************************/
void UserProc::branchAnalysis() {
    Boomerang::get()->alert_decompile_debug_point(this, "before branch analysis.");

    StatementList stmts;
    getStatements(stmts);
    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++) {
        Statement *stmt = *it;
        if (stmt->isBranch()) {
            BranchStatement *branch = (BranchStatement*)stmt;
            if (branch->getFallBB() && branch->getTakenBB()) {
                StatementList fallstmts;
                branch->getFallBB()->getStatements(fallstmts);
                if (fallstmts.size() == 1 && (*fallstmts.begin())->isBranch()) {
                    BranchStatement *fallto = (BranchStatement*)*fallstmts.begin();
                    //   branch to A if cond1
                    //   branch to B if cond2
                    // A: something
                    // B:
                    // ->
                    //   branch to B if !cond1 && cond2
                    // A: something
                    // B:
                    if (fallto->getFallBB() == branch->getTakenBB() && fallto->getBB()->getNumInEdges() == 1) {
                        branch->setFallBB(fallto->getFallBB());
                        branch->setTakenBB(fallto->getTakenBB());
                        branch->setDest(fallto->getFixedDest());
                        Exp* cond = new Binary(opAnd,
                                               new Unary(opNot, branch->getCondExpr()),
                                               fallto->getCondExpr()->clone());
                        branch->setCondExpr(cond->simplify());
                        assert(fallto->getBB()->getNumInEdges() == 0);
                        fallto->getBB()->deleteEdge(fallto->getBB()->getOutEdge(0));
                        fallto->getBB()->deleteEdge(fallto->getBB()->getOutEdge(0));
                        assert(fallto->getBB()->getNumOutEdges() == 0);
                        cfg->removeBB(fallto->getBB());
                    }
                    //   branch to B if cond1
                    //   branch to B if cond2
                    // A: something
                    // B:
                    // ->
                    //   branch to B if cond1 || cond2
                    // A: something
                    // B:
                    if (fallto->getTakenBB() == branch->getTakenBB() && fallto->getBB()->getNumInEdges() == 1) {
                        branch->setFallBB(fallto->getFallBB());
                        branch->setCondExpr(new Binary(opOr,
                                                       branch->getCondExpr(),
                                                       fallto->getCondExpr()->clone()));
                        assert(fallto->getBB()->getNumInEdges() == 0);
                        fallto->getBB()->deleteEdge(fallto->getBB()->getOutEdge(0));
                        fallto->getBB()->deleteEdge(fallto->getBB()->getOutEdge(0));
                        assert(fallto->getBB()->getNumOutEdges() == 0);
                        cfg->removeBB(fallto->getBB());
                    }

                }
            }
        }
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after branch analysis.");
}

/***************************************************************************//**
 *
 * \brief Fix any ugly branch statements (from propagating too much)
 *
 ******************************************************************************/
void UserProc::fixUglyBranches() {
    if (VERBOSE)
        LOG << "### fixUglyBranches for " << getName() << " ###\n";

    StatementList stmts;
    getStatements(stmts);
    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++) {
        Statement *stmt = *it;
        if (stmt->isBranch()) {
            Exp *hl = ((BranchStatement*)stmt)->getCondExpr();
            // of the form: x{n} - 1 >= 0
            if (hl && hl->getOper() == opGtrEq &&
                    hl->getSubExp2()->isIntConst() &&
                    ((Const*)hl->getSubExp2())->getInt() == 0 &&
                    hl->getSubExp1()->getOper() == opMinus &&
                    hl->getSubExp1()->getSubExp2()->isIntConst() &&
                    ((Const*)hl->getSubExp1()->getSubExp2())->getInt() == 1 &&
                    hl->getSubExp1()->getSubExp1()->isSubscript()) {
                Statement *n = ((RefExp*)hl->getSubExp1()->getSubExp1())->getDef();
                if (n && n->isPhi()) {
                    PhiAssign *p = (PhiAssign*)n;
                    for (int i = 0; i < p->getNumDefs(); i++)
                        if (p->getStmtAt(i)->isAssign()) {
                            Assign *a = (Assign*)p->getStmtAt(i);
                            if (*a->getRight() == *hl->getSubExp1()) {
                                hl->setSubExp1(new RefExp(a->getLeft(), a));
                                break;
                            }
                        }
                }
            }
        }
    }

    if (VERBOSE) {
        LOG << "--- after fixUglyBranches for " << getName() << "\n"
            << *this
            << "=== after fixUglyBranches for " << getName() << "\n";
    }
}

/***************************************************************************//**
 *
 * \brief Rename block variables, with log if verbose.
 * \returns true if a change
 *
 ******************************************************************************/
bool UserProc::doRenameBlockVars(int pass, bool clearStacks) {
    if (VERBOSE)
        LOG << "### rename block vars for " << getName() << " pass " << pass << ", clear = " << clearStacks << " ###\n";
    bool b = df.renameBlockVars(this, 0, clearStacks);
    if (VERBOSE)
        LOG << "df.renameBlockVars return " << (b ? "true" : "false") << "\n";
    return b;
}

/***************************************************************************//**
 *
 * \brief Preservations only for the stack pointer
 *
 ******************************************************************************/
void UserProc::findSpPreservation() {
    if (VERBOSE)
        LOG << "finding stack pointer preservation for " << getName() << "\n";

    bool stdsp = false;        // FIXME: are these really used?
    // Note: need this non-virtual version most of the time, since nothing proved yet
    int sp = signature->getStackRegister(prog);

    for (int n = 0; n < 2; n++) {
        // may need to do multiple times due to dependencies FIXME: efficiency! Needed any more?

        // Special case for 32-bit stack-based machines (e.g. Pentium).
        // RISC machines generally preserve the stack pointer (so no special case required)
        for (int p = 0; !stdsp && p < 8; p++) {
            if (DEBUG_PROOF)
                LOG << "attempting to prove sp = sp + " << p*4 << " for " << getName() << "\n";
            stdsp = prove(new Binary(opEquals,
                                     Location::regOf(sp),
                                     new Binary(opPlus,
                                                Location::regOf(sp),
                                                new Const(p * 4))));
        }
    }

    if (DEBUG_PROOF) {
        LOG << "proven for " << getName() << ":\n";
        for (std::map<Exp*, Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); it++)
            LOG << it->first << " = " << it->second << "\n";
    }

}

/***************************************************************************//**
 *
 * \brief  Was trimReturns()
 *
 ******************************************************************************/
void UserProc::findPreserveds() {
    std::set<Exp*> removes;

    if (VERBOSE)
        LOG << "finding preserveds for " << getName() << "\n";

    Boomerang::get()->alert_decompile_debug_point(this, "before finding preserveds");

    if (theReturnStatement == nullptr) {
        if (DEBUG_PROOF)
            LOG << "can't find preservations as there is no return statement!\n";
        Boomerang::get()->alert_decompile_debug_point(this, "after finding preserveds (no return)");
        return;
    }

    // prove preservation for all modifieds in the return statement
    ReturnStatement::iterator mm;
    StatementList& modifieds = theReturnStatement->getModifieds();
    for (mm = modifieds.begin(); mm != modifieds.end(); ++mm) {
        Exp* lhs = ((Assignment*)*mm)->getLeft();
        Exp* equation = new Binary(opEquals, lhs, lhs);
        if (DEBUG_PROOF)
            LOG << "attempting to prove " << equation << " is preserved by " << getName() << "\n";
        if (prove(equation)) {
            removes.insert(equation);
        }
    }

    if (DEBUG_PROOF) {
        LOG << "### proven true for procedure " << getName() << ":\n";
        for (std::map<Exp*, Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); it++)
            LOG << it->first << " = " << it->second << "\n";
        LOG << "### end proven true for procedure " << getName() << "\n\n";
#if PROVEN_FALSE
        LOG << "### proven false for procedure " << getName() << ":\n";
        for (std::map<Exp*, Exp*, lessExpStar>::iterator it = provenFalse.begin(); it != provenFalse.end(); it++)
            LOG << it->first << " != " << it->second << "\n";
        LOG << "### end proven false for procedure " << getName() << "\n\n";
#endif
    }

    // Remove the preserved locations from the modifieds and the returns
    std::map<Exp*, Exp*, lessExpStar>::iterator pp;
    for (pp = provenTrue.begin(); pp != provenTrue.end(); ++pp) {
        Exp* lhs = pp->first;
        Exp* rhs = pp->second;
        // Has to be of the form loc = loc, not say loc+4, otherwise the bypass logic won't see the add of 4
        if (!(*lhs == *rhs)) continue;
        theReturnStatement->removeModified(lhs);
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after finding preserveds");
}

void UserProc::removeSpAssignsIfPossible() {
    // if there are no uses of sp other than sp{-} in the whole procedure,
    // we can safely remove all assignments to sp, this will make the output
    // more readable for human eyes.

    Exp *sp = Location::regOf(signature->getStackRegister(prog));
    bool foundone = false;

    StatementList stmts;
    getStatements(stmts);
    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++) {
        Statement *stmt = *it;
        if (stmt->isAssign() && *((Assign*)stmt)->getLeft() == *sp)
            foundone = true;
        LocationSet refs;
        stmt->addUsedLocs(refs);
        LocationSet::iterator rr;
        for (rr = refs.begin(); rr != refs.end(); rr++)
            if ((*rr)->isSubscript() && *(*rr)->getSubExp1() == *sp) {
                Statement *def = ((RefExp*)(*rr))->getDef();
                if (def && def->getProc() == this)
                    return;
            }
    }

    if (!foundone)
        return;

    Boomerang::get()->alert_decompile_debug_point(this, "before removing stack pointer assigns.");

    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++)
        if ((*it)->isAssign()) {
            Assign *a = (Assign*)*it;
            if (*a->getLeft() == *sp) {
                removeStatement(a);
            }
        }

    Boomerang::get()->alert_decompile_debug_point(this, "after removing stack pointer assigns.");
}

void UserProc::removeMatchingAssignsIfPossible(Exp *e) {
    // if there are no uses of %flags in the whole procedure,
    // we can safely remove all assignments to %flags, this will make the output
    // more readable for human eyes and makes short circuit analysis easier.

    bool foundone = false;

    StatementList stmts;
    getStatements(stmts);
    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++) {
        Statement *stmt = *it;
        if (stmt->isAssign() && *((Assign*)stmt)->getLeft() == *e)
            foundone = true;
        if (stmt->isPhi()) {
            if (*((PhiAssign*)stmt)->getLeft() == *e)
                foundone = true;
            continue;
        }
        LocationSet refs;
        stmt->addUsedLocs(refs);
        LocationSet::iterator rr;
        for (rr = refs.begin(); rr != refs.end(); rr++)
            if ((*rr)->isSubscript() && *(*rr)->getSubExp1() == *e) {
                Statement *def = ((RefExp*)(*rr))->getDef();
                if (def && def->getProc() == this)
                    return;
            }
    }

    if (!foundone)
        return;

    std::ostringstream str;
    str << "before removing matching assigns (" << e << ").";
    Boomerang::get()->alert_decompile_debug_point(this, str.str().c_str());
    if (VERBOSE)
        LOG << str.str().c_str() << "\n";

    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++)
        if ((*it)->isAssign()) {
            Assign *a = (Assign*)*it;
            if (*a->getLeft() == *e)
                removeStatement(a);
        } else if ((*it)->isPhi()) {
            PhiAssign *a = (PhiAssign*)*it;
            if (*a->getLeft() == *e)
                removeStatement(a);
        }

    str.str("");
    str << "after removing matching assigns (" << e << ").";
    Boomerang::get()->alert_decompile_debug_point(this, str.str().c_str());
    LOG << str.str() << "\n";

}

/*
 * Find the procs the calls point to.
 * To be called after decoding all procs.
 * was in: analyis.cpp
 */
//! find the procs the calls point to
void UserProc::assignProcsToCalls() {
    std::list<BasicBlock *>::iterator it;
    BasicBlock * pBB = cfg->getFirstBB(it);
    while (pBB) {
        std::list<RTL*>* rtls = pBB->getRTLs();
        if (rtls == nullptr) {
            pBB = cfg->getNextBB(it);
            continue;
        }
        for (std::list<RTL*>::iterator it2 = rtls->begin(); it2 != rtls->end(); it2++) {
            if (!(*it2)->isCall()) continue;
            CallStatement* call = (CallStatement*)(*it2)->back();
            if (call->getDestProc() == nullptr && !call->isComputed()) {
                Proc *p = prog->findProc(call->getFixedDest());
                if (p == nullptr) {
                    std::cerr << "Cannot find proc for dest " << call->getFixedDest() << " in call at "
                              << (*it2)->getAddress() << "\n";
                    assert(p);
                }
                call->setDestProc(p);
            }
            // call->setSigArguments();        // But BBs not set yet; will get done in initStatements()
        }

        pBB = cfg->getNextBB(it);
    }
}

/*
 * Perform final simplifications
 * was in: analyis.cpp
 */
//! perform final simplifications
void UserProc::finalSimplify() {
    std::list<BasicBlock *>::iterator it;
    BasicBlock * pBB = cfg->getFirstBB(it);
    while (pBB) {
        std::list<RTL*>* pRtls = pBB->getRTLs();
        if (pRtls == nullptr) {
            pBB = cfg->getNextBB(it);
            continue;
        }
        for (RTL * rit : *pRtls) {
            for (Statement* rt : *rit) {
                rt->simplifyAddr();
                // Also simplify everything; in particular, stack offsets are
                // often negative, so we at least canonicalise [esp + -8] to [esp-8]
                rt->simplify();
            }
        }
        pBB = cfg->getNextBB(it);
    }
}


// m[WILD]{-}
static RefExp *memOfWild = new RefExp(
                               Location::memOf(new Terminal(opWild)), nullptr);
// r[WILD INT]{-}
static RefExp* regOfWild = new RefExp(
                               Location::regOf(new Terminal(opWildIntConst)), nullptr);

// Search for expressions without explicit definitions (i.e. WILDCARD{0}), which represent parameters (use before
// definition).
// These are called final parameters, because they are determined from implicit references, not from the use collector
// at the start of the proc, which include some caused by recursive calls
#define DEBUG_PARAMS 1
void UserProc::findFinalParameters() {

    Boomerang::get()->alert_decompile_debug_point(this, "before find final parameters.");

    parameters.clear();

    if (signature->isForced()) {
        // Copy from signature
        int n = signature->getNumParams();
        ImplicitConverter ic(cfg);
        for (int i=0; i < n; ++i) {
            Exp* paramLoc = signature->getParamExp(i)->clone();        // E.g. m[r28 + 4]
            LocationSet components;
            LocationSet::iterator cc;
            paramLoc->addUsedLocs(components);
            for (cc = components.begin(); cc != components.end(); ++cc) {
                if (*cc != paramLoc) {                                // Don't subscript outer level
                    paramLoc->expSubscriptVar(*cc, nullptr);            // E.g. r28 -> r28{-}
                    paramLoc->accept(&ic);                            // E.g. r28{-} -> r28{0}
                }
            }
            ImplicitAssign* ia = new ImplicitAssign(signature->getParamType(i), paramLoc);
            parameters.append(ia);
            const char* name = signature->getParamName(i);
            Exp* param = Location::param(name, this);
            RefExp* reParamLoc = new RefExp(paramLoc, cfg->findImplicitAssign(paramLoc));
            mapSymbolTo(reParamLoc, param);                            // Update name map
        }
        return;
    }
    if (VERBOSE || DEBUG_PARAMS)
        LOG << "finding final parameters for " << getName() << "\n";

    //    int sp = signature->getStackRegister();
    signature->setNumParams(0);            // Clear any old ideas
    StatementList stmts;
    getStatements(stmts);

    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); ++it) {
        Statement* s = *it;
        // Assume that all parameters will be m[]{0} or r[]{0}, and in the implicit definitions at the start of the
        // program
        if (!s->isImplicit())
            // Note: phis can get converted to assignments, but I hope that this is only later on: check this!
            break;                    // Stop after reading all implicit assignments
        Exp *e = ((ImplicitAssign*)s)->getLeft();
        if (signature->findParam(e) == -1) {
            if (VERBOSE || DEBUG_PARAMS)
                LOG << "potential param " << e << "\n";
#if 1        // I believe that the only true parameters will be registers or memofs that look like locals (stack
            // pararameters)
            if (!(e->isRegOf() || isLocalOrParamPattern(e)))
                continue;
#else
            if (signature->isStackLocal(prog, e) || e->getOper() == opLocal) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring local " << e << "\n";
                continue;
            }
            if (e->isGlobal()) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring global " << e << "\n";
                continue;
            }
            if (e->getMemDepth() > 1) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring complex " << e << "\n";
                continue;
            }
            if (e->isMemOf() && e->getSubExp1()->isGlobal()) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring m[global] " << e << "\n";
                continue;
            }
            if (e->isMemOf() && e->getSubExp1()->getOper() == opParam) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring m[param] " << e << "\n";
                continue;
            }
            if (e->isMemOf() &&
                    e->getSubExp1()->getOper() == opPlus &&
                    e->getSubExp1()->getSubExp1()->isGlobal() &&
                    e->getSubExp1()->getSubExp2()->isIntConst()) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring m[global + int] " << e << "\n";
                continue;
            }
            if (e->isRegN(sp)) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring stack pointer register\n";
                continue;
            }
            if (e->isMemOf() && e->getSubExp1()->isConst()) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring m[const]\n";
                continue;
            }
            bool allZero;
            e->clone()->removeSubscripts(allZero);
            if (!allZero) {
                if (VERBOSE || DEBUG_PARAMS)
                    LOG << "ignoring not all-zero\n";
                continue;
            }
#endif
            if (VERBOSE || DEBUG_PARAMS)
                LOG << "found new parameter " << e << "\n";

            Type* ty = ((ImplicitAssign*)s)->getType();
            // Add this parameter to the signature (for now; creates parameter names)
            addParameter(e, ty);
            // Insert it into the parameters StatementList, in sensible order
            insertParameter(e, ty);
        }
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after find final parameters.");
}

#if 0        // FIXME: not currently used; do we want this any more?
//! Trim parameters. If depth not given or == -1, perform at all depths
void UserProc::trimParameters(int depth) {

    if (signature->isForced())
        return;

    if (VERBOSE)
        LOG << "trimming parameters for " << getName() << "\n";

    StatementList stmts;
    getStatements(stmts);

    // find parameters that are referenced (ignore calls to this)
    int nparams = signature->getNumParams();
    int totparams = nparams;
    std::vector<Exp*> params;
    bool referenced[64];
    assert(totparams <= (int)(sizeof(referenced)/sizeof(bool)));
    int i;
    for (i = 0; i < nparams; i++) {
        referenced[i] = false;
        // Push parameters implicitly defined e.g. m[r28{0}+8]{0}, (these are the parameters for the current proc)
        params.push_back(signature->getParamExp(i)->clone()->expSubscriptAllNull());
    }

    std::set<Statement*> excluded;
    StatementList::iterator it;

    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        if (!s->isCall() || ((CallStatement*)s)->getDestProc() != this) {
            for (int i = 0; i < totparams; i++) {
                Exp *p, *pe;
                //if (i < nparams) {
                p = Location::param(signature->getParamName(i), this);
                pe = signature->getParamExp(i);
                //} else {
                //    p = Location::param(signature->getImplicitParamName( i - nparams), this);
                //    pe = signature->getImplicitParamExp(i - nparams);
                //}
                if (!referenced[i] && excluded.find(s) == excluded.end() &&
                        // Search for the named parameter (e.g. param1), and just in case, also for the expression
                        // (e.g. r8{0})
                        (s->usesExp(p) || s->usesExp(params[i]))) {
                    referenced[i] = true;
                    if (DEBUG_UNUSED) {
                        LOG << "parameter " << p << " used by statement " << s->getNumber() << " : " << s->getKind() <<
                               "\n";
                    }
                }
                if (!referenced[i] && excluded.find(s) == excluded.end() &&
                        s->isPhi() && *((PhiAssign*)s)->getLeft() == *pe) {
                    if (DEBUG_UNUSED)
                        LOG << "searching " << s << " for uses of " << params[i] << "\n";
                    PhiAssign *pa = (PhiAssign*)s;
                    PhiAssign::iterator it1;
                    for (it1 = pa->begin(); it1 != pa->end(); it1++)
                        if (it1->def == nullptr) {
                            referenced[i] = true;
                            if (DEBUG_UNUSED)
                                LOG << "parameter " << p << " used by phi statement " << s->getNumber() << "\n";
                            break;
                        }
                }
                // delete p;
            }
        }
    }

    for (i = 0; i < totparams; i++) {
        if (!referenced[i] && (depth == -1 || params[i]->getMemDepth() == depth)) {
            bool allZero;
            Exp *e = params[i]->removeSubscripts(allZero);
            if (VERBOSE)
                LOG << "removing unused parameter " << e << "\n";
            removeParameter(e);
        }
    }
}
#endif

void UserProc::removeReturn(Exp *e) {
    if (theReturnStatement)
        theReturnStatement->removeReturn(e);
}

void Proc::removeParameter(Exp *e) {
    int n = signature->findParam(e);
    if (n != -1) {
        signature->removeParameter(n);
        for (std::set<CallStatement*>::iterator it = callerSet.begin(); it != callerSet.end(); it++) {
            if (DEBUG_UNUSED)
                LOG << "removing argument " << e << " in pos " << n << " from " << *it << "\n";
            (*it)->removeArgument(n);
        }
    }
}

void Proc::removeReturn(Exp *e) {
    signature->removeReturn(e);
}

//! Add the parameter to the signature
void UserProc::addParameter(Exp *e, Type* ty) {
    // In case it's already an implicit argument:
    removeParameter(e);

    signature->addParameter(e, ty);
}

void UserProc::processFloatConstants() {
    StatementList stmts;
    getStatements(stmts);

    Exp *match = new Ternary(opFsize,
                             new Terminal(opWild),
                             new Terminal(opWild),
                             Location::memOf(new Terminal(opWild)));

    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement *s = *it;

        std::list<Exp*> results;
        s->searchAll(match, results);
        for (std::list<Exp*>::iterator it1 = results.begin(); it1 != results.end(); it1++) {
            Ternary *fsize = (Ternary*) *it1;
            if (fsize->getSubExp3()->getOper() == opMemOf &&
                    fsize->getSubExp3()->getSubExp1()->getOper() == opIntConst) {
                Exp *memof = fsize->getSubExp3();
                ADDRESS u = ((Const*)memof->getSubExp1())->getAddr();
                bool ok;
                double d = prog->getFloatConstant(u, ok);
                if (ok) {
                    LOG << "replacing " << memof << " with " << d << " in " << fsize << "\n";
                    fsize->setSubExp3(new Const(d));
                }
            }
        }
        s->simplify();
    }
}


void UserProc::addParameterSymbols() {
    StatementList::iterator it;
    ImplicitConverter ic(cfg);
    int i=0;
    for (it = parameters.begin(); it != parameters.end(); ++it, ++i) {
        Exp* lhs = ((Assignment*)*it)->getLeft();
        lhs = lhs->expSubscriptAllNull();
        lhs = lhs->accept(&ic);
        Location* to = Location::param(strdup(signature->getParamName(i)), this);
        mapSymbolTo(lhs, to);
    }
}

// Return an expression that is equivilent to e in terms of symbols. Creates new symbols as needed.
/**
 * Return an expression that is equivilent to e in terms of local variables.  Creates new locals as needed.
 */
Exp *UserProc::getSymbolExp(Exp *le, Type *ty, bool lastPass) {
    Exp *e = nullptr;

    // check for references to the middle of a local
    if (    le->isMemOf() &&
            le->getSubExp1()->getOper() == opMinus &&
            le->getSubExp1()->getSubExp1()->isSubscript() &&
            le->getSubExp1()->getSubExp1()->getSubExp1()->isRegN(signature->getStackRegister()) &&
            le->getSubExp1()->getSubExp2()->isIntConst()) {
        for (SymbolMap::iterator it = symbolMap.begin(); it != symbolMap.end(); it++) {
            if ((*it).second->isLocal()) {
                const char *nam = ((Const*)(*it).second->getSubExp1())->getStr();
                if (locals.find(nam) != locals.end()) {
                    Type *lty = locals[nam];
                    const Exp *loc = (*it).first;
                    if (    loc->isMemOf() &&
                            loc->getSubExp1()->getOper() == opMinus &&
                            loc->getSubExp1()->getSubExp1()->isSubscript() &&
                            loc->getSubExp1()->getSubExp1()->getSubExp1()->isRegN(signature->getStackRegister()) &&
                            loc->getSubExp1()->getSubExp2()->isIntConst()) {
                        int n = -((const Const*)loc->getSubExp1()->getSubExp2())->getInt();
                        int m = -((Const*)le->getSubExp1()->getSubExp2())->getInt();
                        if (m > n && m < n + (int)(lty->getSize() / 8)) {
                            e = Location::memOf(
                                    new Binary(opPlus,
                                               new Unary(opAddrOf, (*it).second->clone()),
                                               new Const(m - n)));
                            if (VERBOSE)
                                LOG << "seems " << le << " is in the middle of " << loc << " returning " << e << "\n";
                            return e;
                        }
                    }
                }
            }
        }
    }

    if (symbolMap.find(le) == symbolMap.end()) {
        if (ty == nullptr) {
            if (lastPass)
                ty = new IntegerType();
            else
                ty = new VoidType();            // HACK MVE
        }

        // the default of just assigning an int type is bad..  if the locals is not an int then assigning it this
        // type early results in aliases to this local not being recognised
        if (ty) {
//            Exp* base = le;
//            if (le->isSubscript())
//                base = ((RefExp*)le)->getSubExp1();
            // NOTE: using base below instead of le does not enhance anything, but causes us to lose def information
            e = newLocal(ty->clone(), le);
            mapSymbolTo(le->clone(), e);
            e = e->clone();
        }
    } else {
#if 0    // This code was assuming that locations only every have one type associated with them. The reality is that
        // compilers and machine language programmers sometimes re-use the same locations with different types.
        // Let type analysis sort out which live ranges have which type
        e = symbolMap[le]->clone();
        if (e->getOper() == opLocal && e->getSubExp1()->getOper() == opStrConst) {
            std::string name = ((Const*)e->getSubExp1())->getStr();
            Type *nty = ty;
            Type *ty = locals[name];
            assert(ty);
            if (nty && !(*ty == *nty) && nty->getSize() > ty->getSize()) {
                // FIXME: should this be a type meeting?
                if (DEBUG_TA)
                    LOG << "getSymbolExp: updating type of " << name.c_str() << " to " << nty->getCtype() << "\n";
                ty = nty;
                locals[name] = ty;
            }
            if (ty->resolvesToCompound()) {
                CompoundType *compound = ty->asCompound();
                if (VERBOSE)
                    LOG << "found reference to first member of compound " << name.c_str() << ": " << le << "\n";
                char* nam = (char*)compound->getName(0);
                if (nam == nullptr) nam = "??";
                return new TypedExp(ty, new Binary(opMemberAccess, e, new Const(nam)));
            }
        }
#else
        e = getSymbolFor(le, ty);
#endif
    }
    return e;
}

// Not used with DFA Type Analysis; the equivalent thing happens in mapLocalsAndParams() now
void UserProc::mapExpressionsToLocals(bool lastPass) {
    StatementList stmts;
    getStatements(stmts);

    Boomerang::get()->alert_decompile_debug_point(this, "before mapping expressions to locals");

    if (VERBOSE) {
        LOG << "mapping expressions to locals for " << getName();
        if (lastPass)
            LOG << " last pass";
        LOG << "\n";
    }

    int sp = signature->getStackRegister(prog);
    if (getProven(Location::regOf(sp)) == nullptr) {
        if (VERBOSE)
            LOG << "can't map locals since sp unproven\n";
        return;        // can't replace if nothing proven about sp
    }

    // start with calls because that's where we have the most types
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        if ((*it)->isCall()) {
            CallStatement *call = (CallStatement*)*it;
            for (int i = 0; i < call->getNumArguments(); i++) {
                Type *ty = call->getArgumentType(i);
                Exp *e = call->getArgumentExp(i);
                // If a pointer type and e is of the form m[sp{0} - K]:
                if (ty && ty->resolvesToPointer() && signature->isAddrOfStackLocal(prog, e)) {
                    LOG << "argument " << e << " is an addr of stack local and the type resolves to a pointer\n";
                    Exp *olde = e->clone();
                    Type *pty = ty->asPointer()->getPointsTo();
                    if (e->isAddrOf() && e->getSubExp1()->isSubscript() &&
                            e->getSubExp1()->getSubExp1()->isMemOf())
                        e = e->getSubExp1()->getSubExp1()->getSubExp1();
                    if (pty->resolvesToArray() && pty->asArray()->isUnbounded()) {
                        ArrayType *a = (ArrayType*)pty->asArray()->clone();
                        pty = a;
                        a->setLength(1024);        // just something arbitrary
                        if (i+1 < call->getNumArguments()) {
                            Type *nt = call->getArgumentType(i+1);
                            if (nt->isNamed())
                                nt = ((NamedType*)nt)->resolvesTo();
                            if (nt->isInteger() && call->getArgumentExp(i+1)->isIntConst())
                                a->setLength(((Const*)call->getArgumentExp(i+1)) ->getInt());
                        }
                    }
                    e = getSymbolExp(Location::memOf(e->clone(), this), pty);
                    if (e) {
                        Exp *ne = new Unary(opAddrOf, e);
                        if (VERBOSE)
                            LOG << "replacing argument " << olde << " with " << ne << " in " << call << "\n";
                        call->setArgumentExp(i, ne);
                    }
                }
            }
        }
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after processing locals in calls");

    // normalise sp usage (turn WILD + sp{0} into sp{0} + WILD)
    Exp *nn = new Binary(opPlus, new Terminal(opWild), new RefExp(Location::regOf(sp), nullptr));
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        std::list<Exp*> results;
        s->searchAll(nn, results);
        for (std::list<Exp*>::iterator it1 = results.begin(); it1 != results.end(); it1++) {
            Exp *wild = (*it1)->getSubExp1();
            (*it1)->setSubExp1((*it1)->getSubExp2());
            (*it1)->setSubExp2(wild);
        }
    }

    // FIXME: this is probably part of the ADHOC TA
    // look for array locals
    // l = m[(sp{0} + WILD1) - K2]
    Exp *l = Location::memOf(new Binary(opMinus,
                                        new Binary(opPlus,
                                                   new RefExp(Location::regOf(sp), nullptr),
                                                   new Terminal(opWild)),
                                        new Terminal(opWildIntConst)));
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        std::list<Exp*> results;
        s->searchAll(l, results);
        for (Exp *result : results) {
            // arr = m[sp{0} - K2]
            Location *arr = Location::memOf(
                                new Binary(opMinus,
                                           new RefExp(Location::regOf(sp), nullptr),
                                           result->getSubExp1()->getSubExp2()->clone()),
                        this);
            int n = ((Const*)result->getSubExp1()->getSubExp2())->getInt();
            Type *base = new IntegerType();
            if (s->isAssign() && ((Assign*)s)->getLeft() == result) {
                Type* at = ((Assign*)s)->getType();
                if(at && at->getSize() != 0)
                    base = ((Assign*)s)->getType()->clone();
            }
            //arr->setType(new ArrayType(base, n / (base->getSize() / 8))); //TODO: why is this commented out ?
            if (VERBOSE)
                LOG << "found a local array using " << n << " bytes\n";
            Exp *replace = Location::memOf(
                               new Binary(opPlus,
                                          new Unary(opAddrOf, arr),
                                          result->getSubExp1()->getSubExp1()->getSubExp2()->clone()), this);
            //TODO: the change from de8c876e9ca33e6f5aab39191204e80b81048d67 doesn't change anything, but 'looks' better
            TypedExp *actual_replacer = new TypedExp(new ArrayType(base, n / (base->getSize() / 8)),replace);
            if (VERBOSE)
                LOG << "replacing " << result << " with " << actual_replacer << " in " << s << "\n";
            s->searchAndReplace(result, actual_replacer);
        }
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after processing array locals");

    // Stack offsets for local variables could be negative (most machines), positive (PA/RISC), or both (SPARC)
    if (signature->isLocalOffsetNegative())
        searchRegularLocals(opMinus, lastPass, sp, stmts);
    if (signature->isLocalOffsetPositive())
        searchRegularLocals(opPlus, lastPass, sp, stmts);
    // Ugh - m[sp] is a special case: neither positive or negative.  SPARC uses this to save %i0
    if (signature->isLocalOffsetPositive() && signature->isLocalOffsetNegative())
        searchRegularLocals(opWild, lastPass, sp, stmts);

    Boomerang::get()->alert_decompile_debug_point(this, "after mapping expressions to locals");
}

void UserProc::searchRegularLocals(OPER minusOrPlus, bool lastPass, int sp, StatementList& stmts) {
    // replace expressions in regular statements with locals
    Location* l;
    if (minusOrPlus == opWild)
        // l = m[sp{0}]
        l = Location::memOf(
                new RefExp(Location::regOf(sp), nullptr));
    else
        // l = m[sp{0} +/- K]
        l = Location::memOf(
                new Binary(minusOrPlus,
                           new RefExp(Location::regOf(sp), nullptr),
                           new Terminal(opWildIntConst)));
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        std::list<Exp*> results;
        s->searchAll(l, results);
        for (std::list<Exp*>::iterator it1 = results.begin(); it1 != results.end(); it1++) {
            Exp *result = *it1;
            Type* ty = s->getTypeFor(result);
            Exp *e = getSymbolExp(result, ty, lastPass);
            if (e) {
                Exp* search = result->clone();
                if (VERBOSE)
                    LOG << "mapping " << search << " to " << e << " in " << s << "\n";
                //s->searchAndReplace(search, e);
            }
        }
        //s->simplify();
    }
}

bool UserProc::removeNullStatements() {
    bool change = false;
    StatementList stmts;
    getStatements(stmts);
    // remove null code
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        if (s->isNullStatement()) {
            // A statement of the form x := x
            if (VERBOSE) {
                LOG << "removing null statement: " << s->getNumber() <<
                       " " << s << "\n";
            }
            removeStatement(s);
            change = true;
        }
    }
    return change;
}

// Propagate statements, but don't remove
// Return true if change; set convert if an indirect call is converted to direct (else clear)
/// Propagate statemtents; return true if change; set convert if an indirect call is converted to direct
/// (else clear)
bool UserProc::propagateStatements(bool& convert, int pass) {
    if (VERBOSE)
        LOG << "--- begin propagating statements pass " << pass << " ---\n";
    StatementList stmts;
    getStatements(stmts);
    // propagate any statements that can be
    StatementList::iterator it;
    // Find the locations that are used by a live, dominating phi-function
    LocationSet usedByDomPhi;
    findLiveAtDomPhi(usedByDomPhi);
    // Next pass: count the number of times each assignment LHS would be propagated somewhere
    std::map<Exp*, int, lessExpStar> destCounts;
    // Also maintain a set of locations which are used by phi statements
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        ExpDestCounter edc(destCounts);
        StmtDestCounter sdc(&edc);
        s->accept(&sdc);
    }
#if USE_DOMINANCE_NUMS
    // A third pass for dominance numbers
    setDominanceNumbers();
#endif
    // A fourth pass to propagate only the flags (these must be propagated even if it results in extra locals)
    bool change = false;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        if (s->isPhi()) continue;
        change |= s->propagateFlagsTo();
    }
    // Finally the actual propagation
    convert = false;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        if (s->isPhi()) continue;
        change |= s->propagateTo(convert, &destCounts, &usedByDomPhi);
    }
    simplify();
    propagateToCollector();
    if (VERBOSE)
        LOG << "=== end propagating statements at pass " << pass << " ===\n";
    return change;
}    // propagateStatements

/***************************************************************************//**
 *
 * \brief Middle decompile: All the decompilation from preservation up to
 * but not including removing unused statements.
 * \returns the cycle set from the recursive call to decompile()
 *
 ******************************************************************************/
Statement *UserProc::getStmtAtLex(unsigned int begin, unsigned int end) {
    StatementList stmts;
    getStatements(stmts);

    unsigned int lowest = begin;
    Statement *loweststmt = nullptr;
    for (StatementList::iterator it = stmts.begin(); it != stmts.end(); it++)
        if (begin >= (*it)->getLexBegin() && begin <= lowest && begin <= (*it)->getLexEnd() &&
                (end == (unsigned)-1 || end < (*it)->getLexEnd())) {
            loweststmt = (*it);
            lowest = (*it)->getLexBegin();
        }
    return loweststmt;
}
/// promote the signature if possible
void UserProc::promoteSignature() {
    signature = signature->promote(this);
}

/// Return a string for a new local suitable for \a e
const char* UserProc::newLocalName(Exp* e) {
    std::ostringstream ost;
    if (e->isSubscript() && ((RefExp*)e)->getSubExp1()->isRegOf()) {
        // Assume that it's better to know what register this location was created from
        const char* regName = getRegName(((RefExp*)e)->getSubExp1());
        int tag = 0;
        do {
            ost.str("");
            ost << regName << "_" << ++tag;
        } while (locals.find(ost.str()) != locals.end());
        return strdup (ost.str().c_str());
    }
    ost << "local" << nextLocal++;
    return strdup(ost.str().c_str());
}
/**
 * Return the next available local variable; make it the given type. Note: was returning TypedExp*.
 * If nam is non null, use that name
 */
Exp* UserProc::newLocal(Type* ty, Exp* e, char* nam /* = nullptr */) {
    std::string name;
    if (nam == nullptr)
        name = newLocalName(e);
    else
        name = nam;                // Use provided name
    locals[name] = ty;
    if (ty == nullptr) {
        std::cerr << "null type passed to newLocal\n";
        assert(false);
    }
    if (VERBOSE)
        LOG << "assigning type " << ty->getCtype() << " to new " << name.c_str() << "\n";
    return Location::local(strdup(name.c_str()), this);
}

/**
 * Add a new local supplying all needed information.
 */
void UserProc::addLocal(Type *ty, const char *nam, Exp *e) {
    // symbolMap is a multimap now; you might have r8->o0 for integers and r8->o0_1 for char*
    //assert(symbolMap.find(e) == symbolMap.end());
    mapSymbolTo(e, Location::local(strdup(nam), this));
    //assert(locals.find(nam) == locals.end());        // Could be r10{20} -> o2, r10{30}->o2 now
    locals[nam] = ty;
}

/// return a local's type
Type *UserProc::getLocalType(const char *nam) {
    if (locals.find(nam) == locals.end())
        return nullptr;
    Type *ty = locals[nam];
    return ty;
}

void UserProc::setLocalType(const char *nam, Type *ty) {
    locals[nam] = ty;
    if (VERBOSE)
        LOG << "setLocalType: updating type of " << nam << " to " << ty->getCtype() << "\n";
}

Type *UserProc::getParamType(const char *nam) {
    for (unsigned int i = 0; i < signature->getNumParams(); i++)
        if (std::string(nam) == signature->getParamName(i))
            return signature->getParamType(i);
    return nullptr;
}

//void UserProc::setExpSymbol(const char *nam, Exp *e, Type* ty) {
//    TypedExp *te = new TypedExp(ty, Location::local(strdup(nam), this));
//    mapSymbolTo(e, te);
//}

/// As above but with replacement
void UserProc::mapSymbolToRepl(const Exp* from, Exp* oldTo, Exp* newTo) {
    removeSymbolMapping(from, oldTo);
    mapSymbolTo(from, newTo);        // The compiler could optimise this call to a fall through
}

void UserProc::mapSymbolTo(const Exp* from, Exp* to) {
    SymbolMap::iterator it = symbolMap.find(from);
    while (it != symbolMap.end() && *it->first == *from) {
        if (*it->second == *to)
            return;                // Already in the multimap
        ++it;
    }
    std::pair<const Exp*, Exp*> pr = {from,to};
    symbolMap.insert(pr);
}

// FIXME: is this the same as lookupSym() now?
/// \brief Lookup the symbol map considering type
/// Lookup the expression in the symbol map. Return nullptr or a C string with the symbol. Use the Type* ty to
/// select from several names in the multimap; the name corresponding to the first compatible type is returned
Exp* UserProc::getSymbolFor(const Exp* from, Type* ty) {
    SymbolMap::iterator ff = symbolMap.find(from);
    while (ff != symbolMap.end() && *ff->first == *from) {
        Exp* currTo = ff->second;
        assert(currTo->isLocal() || currTo->isParam());
        const char* name = ((Const*)((Location*)currTo)->getSubExp1())->getStr();
        Type* currTy = getLocalType(name);
        if (currTy == nullptr) currTy = getParamType(name);
        if (currTy && currTy->isCompatibleWith(ty))
            return currTo;
        ++ff;
    }
    return nullptr;
}
/// Remove this mapping
void UserProc::removeSymbolMapping(const Exp* from, Exp* to) {
    SymbolMap::iterator it = symbolMap.find(from);
    while (it != symbolMap.end() && *it->first == *from) {
        if (*it->second == *to) {
            symbolMap.erase(it);
            return;
        }
        it++;
    }
}



/// return a symbol's exp (note: the original exp, like r24, not local1)
/// \note linear search!!
const Exp *UserProc::expFromSymbol(const char *nam) const {
    for (const std::pair<const Exp*,Exp*> & it : symbolMap) {
        const Exp* e = it.second;
        if (e->isLocal() && !strcmp(((const Const*)((const Location*)e)->getSubExp1())->getStr(), nam))
            return it.first;
    }
    return nullptr;
}

const char* UserProc::getLocalName(int n) {
    int i = 0;
    for (std::map<std::string, Type*>::iterator it = locals.begin(); it != locals.end(); it++, i++)
        if (i == n)
            return it->first.c_str();
    return nullptr;
}
//! As getLocalName, but look for expression \a e
const char* UserProc::getSymbolName(Exp* e) {
    SymbolMap::iterator it = symbolMap.find(e);
    if (it == symbolMap.end()) return nullptr;
    Exp* loc = it->second;
    if (!loc->isLocal() && !loc->isParam()) return nullptr;
    return ((Const*)loc->getSubExp1())->getStr();
}


// Count references to the things that are under SSA control. For each SSA subscripting, increment a counter for that
// definition
void UserProc::countRefs(RefCounter& refCounts) {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        // Don't count uses in implicit statements. There is no RHS of course, but you can still have x from m[x] on the
        // LHS and so on, and these are not real uses
        if (s->isImplicit()) continue;
        if (DEBUG_UNUSED)
            LOG << "counting references in " << s << "\n";
        LocationSet refs;
        s->addUsedLocs(refs, false);            // Ignore uses in collectors
        LocationSet::iterator rr;
        for (rr = refs.begin(); rr != refs.end(); rr++) {
            if (((Exp*)*rr)->isSubscript()) {
                Statement *def = ((RefExp*)*rr)->getDef();
                // Used to not count implicit refs here (def->getNumber() == 0), meaning that implicit definitions get
                // removed as dead code! But these are the ideal place to read off final parameters, and it is
                // guaranteed now that implicit statements are sorted out for us by now (for dfa type analysis)
                if (def /* && def->getNumber() */ ) {
                    refCounts[def]++;
                    if (DEBUG_UNUSED)
                        LOG << "counted ref to " << *rr << "\n";
                }
            }
        }
    }
    if (DEBUG_UNUSED) {
        RefCounter::iterator rr;
        LOG << "### reference counts for " << getName() << ":\n";
        for (rr = refCounts.begin(); rr != refCounts.end(); ++rr)
            LOG << "  " << rr->first->getNumber() << ":" << rr->second << "\t";
        LOG << "\n### end reference counts\n";
    }
}

// Note: call the below after translating from SSA form
// FIXME: this can be done before transforming out of SSA form now, surely...
void UserProc::removeUnusedLocals() {
    Boomerang::get()->alert_decompile_debug_point(this, "before removing unused locals");
    if (VERBOSE)
        LOG << "removing unused locals (final) for " << getName() << "\n";

    std::set<std::string> usedLocals;
    StatementList stmts;
    getStatements(stmts);
    // First count any uses of the locals
    StatementList::iterator ss;
    bool all = false;
    for (ss = stmts.begin(); ss != stmts.end(); ss++) {
        Statement* s = *ss;
        LocationSet locs;
        all |= s->addUsedLocals(locs);
        LocationSet::iterator uu;
        for (uu = locs.begin(); uu != locs.end(); uu++) {
            Exp* u = *uu;
            // Must be a real symbol, and not defined in this statement, unless it is a return statement (in which case
            // it is used outside this procedure), or a call statement. Consider local7 = local7+1 and
            // return local7 = local7+1 and local7 = call(local7+1), where in all cases, local7 is not used elsewhere
            // outside this procedure. With the assign, it can be deleted, but with the return or call statements, it
            // can't.
            if ((s->isReturn() || s->isCall() || !s->definesLoc(u))) {
                if (!u->isLocal()) continue;
                std::string name(((Const*)((Location*)u)->getSubExp1())->getStr());
                usedLocals.insert(name);
                if (DEBUG_UNUSED)
                    LOG << "counted local " << name << " in " << s << "\n";
            }
        }
        if (s->isAssignment() && !s->isImplicit() && ((Assignment*)s)->getLeft()->isLocal()) {
            Assignment* as = (Assignment*)s;
            Const* c = (Const*)((Unary*)as->getLeft())->getSubExp1();
            std::string name(c->getStr());
            usedLocals.insert(name);
            if (DEBUG_UNUSED) LOG << "counted local " << name.c_str() << " on left of " << s << "\n";

        }
    }
    // Now record the unused ones in set removes
    std::map<std::string, Type*>::iterator it;
    std::set<std::string> removes;
    for (it = locals.begin(); it != locals.end(); it++) {
        std::string& name = const_cast<std::string&>(it->first);
        // LOG << "Considering local " << name << "\n";
        if (VERBOSE && all && removes.size())
            LOG << "WARNING: defineall seen in procedure " << name.c_str() << " so not removing " << (int)removes.size()
                << " locals\n";
        if (usedLocals.find(name) == usedLocals.end() && !all) {
            if (VERBOSE)
                LOG << "removed unused local " << name.c_str() << "\n";
            removes.insert(name);
        }
    }
    // Remove any definitions of the removed locals
    for (ss = stmts.begin(); ss != stmts.end(); ++ss) {
        Statement* s = *ss;
        LocationSet ls;
        LocationSet::iterator ll;
        s->getDefinitions(ls);
        for (ll = ls.begin(); ll != ls.end(); ++ll) {
            Type* ty = s->getTypeFor(*ll);
            const char* name = findLocal(*ll, ty);
            if (name == nullptr) continue;
            std::string str(name);
            if (removes.find(str) != removes.end()) {
                // Remove it. If an assign, delete it; otherwise (call), remove the define
                if (s->isAssignment()) {
                    removeStatement(s);
                    break;                // Break to next statement
                } else if (s->isCall())
                    // Remove just this define. May end up removing several defines from this call.
                    ((CallStatement*)s)->removeDefine(*ll);
                // else if a ReturnStatement, don't attempt to remove it. The definition is used *outside* this proc.
            }
        }
    }
    // Finally, remove them from locals, so they don't get declared
    for (std::set<std::string>::iterator it1 = removes.begin(); it1 != removes.end(); )
        locals.erase(*it1++);
    // Also remove them from the symbols, since symbols are a superset of locals at present
    for (SymbolMap::iterator sm = symbolMap.begin(); sm != symbolMap.end(); ) {
        Exp* mapsTo = sm->second;
        if (mapsTo->isLocal()) {
            const char* tmpName = ((Const*)((Location*)mapsTo)->getSubExp1())->getStr();
            if (removes.find(tmpName) != removes.end()) {
                symbolMap.erase(sm++);
                continue;
            }
        }
        ++sm;            // sm is itcremented with the erase, or here
    }
    Boomerang::get()->alert_decompile_debug_point(this, "after removing unused locals");
}

//
//    SSA code
//

void UserProc::fromSSAform() {
    Boomerang::get()->alert_decompiling(this);

    if (VERBOSE)
        LOG << "transforming " << getName() << " from SSA\n";

    Boomerang::get()->alert_decompile_debug_point(this, "before transforming from SSA form");

    if (cfg->getNumBBs() >= 100)        // Only for the larger procs
        // Note: emit newline at end of this proc, so we can distinguish getting stuck in this proc with doing a lot of
        // little procs that don't get messages. Also, looks better with progress dots
        std::cout << " transforming out of SSA form " << getName() << " with " << cfg->getNumBBs() << " BBs";

    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;

    for (it = stmts.begin(); it != stmts.end(); it++) {
        // Map registers to initial local variables
        (*it)->mapRegistersToLocals();
        // Insert casts where needed, as types are about to become inaccessible
        (*it)->insertCasts();
    }


    // First split the live ranges where needed by reason of type incompatibility, i.e. when the type of a subscripted
    // variable is different to its previous type. Start at the top, because we don't want to rename parameters (e.g.
    // argc)
    typedef std::pair<Type*, Exp*> FirstTypeEnt;
    typedef std::map<Exp*, FirstTypeEnt, lessExpStar> FirstTypesMap;
    FirstTypesMap firstTypes;
    FirstTypesMap::iterator ff;
    ConnectionGraph ig;                // The interference graph; these can't have the same local variable
    ConnectionGraph pu;                // The Phi Unites: these need the same local variable or copies
#if 0
    // Start with the parameters. There is not always a use of every parameter, yet that location may be used with
    // a different type (e.g. envp used as int in test/sparc/fibo-O4)
    int n = signature->getNumParams();

    for (int i=0; i < n; i++) {
        Exp* paramExp = signature->getParamExp(i);
        FirstTypeEnt fte;
        fte.first = signature->getParamType(i);
        fte.second = new RefExp(paramExp, cfg->findTheImplicitAssign(paramExp));
        firstTypes[paramExp] = fte;
    }
#endif
    int progress = 0;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        if (++progress > 2000) {
            std::cout << "." << std::flush;
            progress = 0;
        }
        Statement* s = *it;
        LocationSet defs;
        s->getDefinitions(defs);
        LocationSet::iterator dd;
        for (dd = defs.begin(); dd != defs.end(); dd++) {
            Exp* base = *dd;
            Type* ty = s->getTypeFor(base);
            if (VERBOSE)
                LOG << "got type " << ty << " for " << base << " from " << s << "\n";
            if (ty == nullptr)                // Can happen e.g. when getting the type for %flags
                ty = new VoidType();
            ff = firstTypes.find(base);
            RefExp* ref = new RefExp(base, s);
            if (ff == firstTypes.end()) {
                // There is no first type yet. Record it.
                FirstTypeEnt fte;
                fte.first = ty;
                fte.second = ref;
                firstTypes[base] = fte;
            } else if (ff->second.first && !ty->isCompatibleWith(ff->second.first)) {
                if (DEBUG_LIVENESS)
                    LOG << "def of " << base << " at " << s->getNumber() << " type " << ty <<
                           " is not compatible with first type " << ff->second.first << ".\n";
                // There already is a type for base, and it is different to the type for this definition.
                // Record an "interference" so it will get a new variable
                if (!ty->isVoid())        // just ignore void interferences ??!!
                    ig.connect(ref, ff->second.second);
            }
        }
    }
    // Find the interferences generated by more than one version of a variable being live at the same program point
    cfg->findInterferences(ig);

    // Find the set of locations that are "united" by phi-functions
    // FIXME: are these going to be trivially predictable?
    findPhiUnites(pu);

    ConnectionGraph::iterator ii;
    if (DEBUG_LIVENESS) {
        LOG << "## ig interference graph:\n";
        for (ii = ig.begin(); ii != ig.end(); ii++)
            LOG << "   ig " << ii->first << " -> " << ii->second << "\n";
        LOG << "## pu phi unites graph:\n";
        for (ii = pu.begin(); ii != pu.end(); ii++)
            LOG << "   pu " << ii->first << " -> " << ii->second << "\n";
        LOG << "  ---\n";
    }

    // Choose one of each interfering location to give a new name to

    for (ii = ig.begin(); ii != ig.end(); ++ii) {
        RefExp* r1, *r2;
        r1 = (RefExp*)ii->first;
        r2 = (RefExp*)ii->second;            // r1 -> r2 and vice versa
        const char* name1 = lookupSymFromRefAny(r1);
        const char* name2 = lookupSymFromRefAny(r2);
        if (name1 && name2 && strcmp(name1, name2) != 0)
            continue;                        // Already different names, probably because of the redundant mapping
        RefExp* rename = nullptr;
        if (r1->isImplicitDef())
            // If r1 is an implicit definition, don't rename it (it is probably a parameter, and should retain its
            // current name)
            rename = r2;
        else if (r2->isImplicitDef())
            rename = r1;         // Ditto for r2
        if (rename == nullptr) {
#if 0        // By preferring to rename the destination of the phi, we have more chance that all the phi operands will
            // end up being the same, so the phi can end up as one copy assignment

            // In general, it is best to rename the location (r1 or r2) which has the smallest number of uniting
            // connections (or at least, this seems like a reasonable criterion)
            int n1 = pu.count(r1);
            int n2 = pu.count(r2);
            if (n2 <= n1)
#else
            Statement* def2 = r2->getDef();
            if (def2->isPhi())            // Prefer the destinations of phis
#endif
                rename = r2;
            else
                rename = r1;
        }
        Type *ty    = rename->getDef()->getTypeFor(rename->getSubExp1());
        Exp* local = newLocal(ty, rename);
        if (DEBUG_LIVENESS)
            LOG << "renaming " << rename << " to " << local << "\n";
        mapSymbolTo(rename, local);
    }

    // Implement part of the Phi Unites list, where renamings or parameters have broken them, by renaming
    // The rest of them will be done as phis are removed
    // The idea is that where l1 and l2 have to unite, and exactly one of them already has a local/name, you can
    // implement the unification by giving the unnamed one the same name as the named one, as long as they don't
    // interfere
    for (ii = pu.begin(); ii != pu.end(); ++ii) {
        RefExp* r1 = (RefExp*)ii->first;
        RefExp* r2 = (RefExp*)ii->second;
        const char* name1 = lookupSymFromRef(r1);
        const char* name2 = lookupSymFromRef(r2);
        if (name1 && !name2 && !ig.isConnected(r1, *r2)) {
            // There is a case where this is unhelpful, and it happen in test/pentium/fromssa2. We have renamed the
            // destination of the phi to ebx_1, and that leaves the two phi operands as ebx. However, we attempt to
            // unite them here, which will cause one of the operands to become ebx_1, so the neat oprimisation of
            // replacing the phi with one copy doesn't work. The result is an extra copy.
            // So check of r1 is a phi and r2 one of its operands, and all other operands for the phi have the same
            // name. If so, don't rename.
            Statement* def1 = r1->getDef();
            if (def1->isPhi()) {
                bool allSame = true;
                bool r2IsOperand = false;
                const char* firstName = nullptr;
                PhiAssign::iterator rr;
                PhiAssign* pi = (PhiAssign*)def1;
                for (rr = pi->begin(); rr != pi->end(); ++rr) {
                    RefExp* re = new RefExp(rr->e, rr->def);
                    if (*re == *r2)
                        r2IsOperand = true;
                    if (firstName == nullptr)
                        firstName = lookupSymFromRefAny(re);
                    else {
                        const char* tmp = lookupSymFromRefAny(re);
                        if (!tmp || strcmp(firstName, tmp) != 0) {
                            allSame = false;
                            break;
                        }
                    }
                }
                if (allSame && r2IsOperand)
                    continue;                        // This situation has happened, don't map now
            }
            mapSymbolTo(r2, Location::local(name1, this));
            continue;
        }
    }


    /*    *    *    *    *    *    *    *    *    *    *    *    *    *    *\
 *                                                        *
 *     IR gets changed with hard locals and params here    *
 *                                                        *
\*    *    *    *    *    *    *    *    *    *    *    *    *    *    */

    // First rename the variables (including phi's, but don't remove).
    // NOTE: it is not possible to postpone renaming these locals till the back end, since the same base location
    // may require different names at different locations, e.g. r28{0} is local0, r28{16} is local1
    // Update symbols and parameters, particularly for the stack pointer inside memofs.
    // NOTE: the ordering of the below operations is critical! Re-ordering may well prevent e.g. parameters from
    // renaming successfully.
    nameParameterPhis();
    mapLocalsAndParams();
    mapParameters();
    removeSubscriptsFromSymbols();
    removeSubscriptsFromParameters();
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        s->replaceSubscriptsWithLocals();
    }

    // Now remove the phis
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        if (!s->isPhi()) continue;
        // Check if the base variables are all the same
        PhiAssign* pa = (PhiAssign*)s;
        if (pa->begin() == pa->end()) {
            // no params to this phi, just remove it
            if (VERBOSE)
                LOG << "phi with no params, removing: " << s << "\n";
            removeStatement(s);
            continue;
        }
        LocationSet refs;
        pa->addUsedLocs(refs);
        bool phiParamsSame = true;
        Exp* first = nullptr;
        if (pa->getNumDefs() > 1) {
            PhiAssign::iterator uu;
            for (uu = pa->begin(); uu != pa->end(); uu++) {
                if (uu->e == nullptr) continue;
                if (first == nullptr) { first = uu->e; continue; }
                if (!(*uu->e == *first)) {
                    phiParamsSame = false;
                    break;
                }
            }
        }
        if (phiParamsSame && first) {
            // Is the left of the phi assignment the same base variable as all the operands?
            if (*pa->getLeft() == *first) {
                if (DEBUG_LIVENESS || DEBUG_UNUSED)
                    LOG << "removing phi: left and all refs same or 0: " << s << "\n";
                // Just removing the refs will work, or removing the whole phi
                // NOTE: Removing the phi here may cause other statments to be not used.
                removeStatement(s);
            } else
                // Need to replace the phi by an expression,
                // e.g. local0 = phi(r24{3}, r24{5}) becomes
                //        local0 = r24
                pa->convertToAssign(first->clone());
        }
        else {
            // Need new local(s) for phi operands that have different names from the lhs
#if 0        // The big problem with this idea is that it extends the range of the phi destination, and it could "pass"
            // definitions and uses of that variable. It could be that the dominance numbers could solve this, but it
            // seems unlikely
            Exp* lhs = pa->getLeft();
            RefExp* wrappedLeft = new RefExp(lhs, pa)
                                  char* lhsName = lookupSymForRef(wrappedLeft);
            PhiAssign::iterator rr;
            for (rr = pa->begin(); rr != pa->end(); rr++) {
                RefExp* operand = new RefExp(rr->e, rr->def);
                char* operName = lookupSymFromRef(operand);
                if (strcmp(operName, lhsName) == 0)
                    continue;                    // No need for copies for this operand
                // Consider possible optimisation here: if have a = phi(b, b, b, b, a), then there is only one copy
                // needed. If by some fluke the program already had a=b after the definition for b, then no copy is
                // needed at all. So a map of existing copies could be useful
                insertAssignAfter(rr->def, lhs, operand);
            }
#else        // This way is costly in copies, but has no problems with extending live ranges
            // Exp* tempLoc = newLocal(pa->getType());
            Exp* tempLoc = getSymbolExp(new RefExp(pa->getLeft(), pa), pa->getType());
            if (DEBUG_LIVENESS)
                LOG << "phi statement " << s << " requires local, using " << tempLoc << "\n";
            // For each definition ref'd in the phi
            PhiAssign::iterator rr;
            for (rr = pa->begin(); rr != pa->end(); rr++) {
                if (rr->e == nullptr) continue;
                insertAssignAfter(rr->def, tempLoc, rr->e);
            }
            // Replace the RHS of the phi with tempLoc
            pa->convertToAssign(tempLoc);
#endif
        }
    }


    if (cfg->getNumBBs() >= 100)        // Only for the larger procs
        std::cout << "\n";

    Boomerang::get()->alert_decompile_debug_point(this, "after transforming from SSA form");
}

void UserProc::mapParameters() {
    // Replace the parameters with their mappings
    StatementList::iterator pp;
    for (pp = parameters.begin(); pp != parameters.end(); ++pp) {
        Exp* lhs = ((Assignment*)*pp)->getLeft();
        const char* mappedName = lookupParam(lhs);
        if (mappedName == nullptr) {
            LOG << "WARNING! No symbol mapping for parameter " << lhs << "\n";
            bool allZero;
            Exp* clean = lhs->clone()->removeSubscripts(allZero);
            if (allZero)
                ((Assignment*)*pp)->setLeft(clean);
            // Else leave them alone
        } else
            ((Assignment*)*pp)->setLeft(Location::param(mappedName, this));
    }
}

void UserProc::removeSubscriptsFromSymbols() {
    // Basically, use the symbol map to map the symbols in the symbol map!
    // However, do not remove subscripts from the outer level; they are still needed for comments in the output and also
    // for when removing subscripts from parameters (still need the {0})
    // Since this will potentially change the ordering of entries, need to copy the map
    SymbolMap sm2 = symbolMap;                // Object copy
    SymbolMap::iterator it;
    symbolMap.clear();
    ExpSsaXformer esx(this);
    for (it = sm2.begin(); it != sm2.end(); ++it) {
        Exp* from = const_cast<Exp *>(it->first);
        if (from->isSubscript()) {
            // As noted above, don't touch the outer level of subscripts
            Exp*& sub = ((RefExp*)from)->refSubExp1();
            sub = sub->accept(&esx);
        } else
            from = from->accept(&esx);
        mapSymbolTo(from, it->second);
    }
}

void UserProc::removeSubscriptsFromParameters() {
    StatementList::iterator it;
    ExpSsaXformer esx(this);
    for (it = parameters.begin(); it != parameters.end(); ++it) {
        Exp* left = ((Assignment*)*it)->getLeft();
        left = left->accept(&esx);
        ((Assignment*)*it)->setLeft(left);
    }
}


static Binary allEqAll(opEquals,
                       new Terminal(opDefineAll),
                       new Terminal(opDefineAll));

// this function was non-reentrant, but now reentrancy is frequently used
/// prove any arbitary property of this procedure. If conditional is true, do not save the result, as it may
/// be conditional on premises stored in other procedures
bool UserProc::prove(Exp *query, bool conditional /* = false */) {

    assert(query->isEquality());
    Exp* queryLeft = ((Binary*)query)->getSubExp1();
    Exp* queryRight = ((Binary*)query)->getSubExp2();
    if (provenTrue.find(queryLeft) != provenTrue.end() && *provenTrue[queryLeft] == *queryRight) {
        if (DEBUG_PROOF) LOG << "found true in provenTrue cache " << query << " in " << getName() << "\n";
        return true;
    }
#if PROVEN_FALSE            // Maybe not so smart... may prove true after some iterations
    if (provenFalse.find(queryLeft) != provenFalse.end() && *provenFalse[queryLeft] == *queryRight) {
        if (DEBUG_PROOF) LOG << "found false in provenFalse cache " << query << " in " << getName() << "\n";
        return false;
    }
#endif

    if (Boomerang::get()->noProve)
        return false;

    Exp *original = query->clone();
    Exp* origLeft = ((Binary*)original)->getSubExp1();
    Exp* origRight = ((Binary*)original)->getSubExp2();


    // subscript locs on the right with {-} (nullptr reference)
    LocationSet locs;
    query->getSubExp2()->addUsedLocs(locs);
    LocationSet::iterator xx;
    for (xx = locs.begin(); xx != locs.end(); xx++) {
        query->setSubExp2(query->getSubExp2()->expSubscriptValNull(*xx));
    }

    if (query->getSubExp1()->getOper() != opSubscript) {
        bool gotDef = false;
        // replace expression from return set with expression in the collector of the return
        if (theReturnStatement) {
            Exp* def = theReturnStatement->findDefFor(query->getSubExp1());
            if (def) {
                query->setSubExp1(def);
                gotDef = true;
            }
        }
        if (!gotDef) {
            // OK, the thing I'm looking for isn't in the return collector, but perhaps there is an entry for <all>
            // If this is proved, then it is safe to say that x == x for any x with no definition reaching the exit
            Exp* right = origRight->clone()->simplify();        // In case it's sp+0
            if (*origLeft == *right &&                            // x == x
                    origLeft->getOper() != opDefineAll &&            // Beware infinite recursion
                    prove(&allEqAll)) {                            // Recurse in case <all> not proven yet
                if (DEBUG_PROOF)
                    LOG << "Using all=all for " << query->getSubExp1() << "\n" << "prove returns true\n";
                provenTrue[origLeft->clone()] = right;
                return true;
            }
            if (DEBUG_PROOF)
                LOG << "not in return collector: " << query->getSubExp1() << "\n" << "prove returns false\n";
            return false;
        }
    }

    if (cycleGrp)            // If in involved in a recursion cycle
        //    then save the original query as a premise for bypassing calls
        recurPremises[origLeft->clone()] = origRight;

    std::set<PhiAssign*> lastPhis;
    std::map<PhiAssign*, Exp*> cache;
    bool result = prover(query, lastPhis, cache, original);
    if (cycleGrp)
        recurPremises.erase(origLeft);            // Remove the premise, regardless of result
    if (DEBUG_PROOF) LOG << "prove returns " << (result ? "true" : "false") << " for " << query << " in " << getName()
                         << "\n";

    if (!conditional) {
        if (result)
            provenTrue[origLeft] = origRight;    // Save the now proven equation
#if PROVEN_FALSE
        else
            provenFalse[origLeft] = origRight;    // Save the now proven-to-be-false equation
#endif
    }
    return result;
}
/// helper function, should be private
bool UserProc::prover(Exp *query, std::set<PhiAssign*>& lastPhis, std::map<PhiAssign*, Exp*> &cache, Exp* original,
                      PhiAssign* lastPhi /* = nullptr */) {
    // A map that seems to be used to detect loops in the call graph:
    std::map<CallStatement*, Exp*> called;
    Exp *phiInd = query->getSubExp2()->clone();

    if (lastPhi && cache.find(lastPhi) != cache.end() && *cache[lastPhi] == *phiInd) {
        if (DEBUG_PROOF)
            LOG << "true - in the phi cache\n";
        return true;
    }

    std::set<Statement*> refsTo;

    query = query->clone();
    bool change = true;
    bool swapped = false;
    while (change) {
        if (DEBUG_PROOF) {
            LOG << query << "\n";
        }

        change = false;
        if (query->getOper() == opEquals) {

            // same left and right means true
            if (*query->getSubExp1() == *query->getSubExp2()) {
                query = new Terminal(opTrue);
                change = true;
            }

            // move constants to the right
            if(!change) {
                Exp *plus = query->getSubExp1();
                Exp *s1s2 = plus ? plus->getSubExp2() : nullptr;
                if(plus && s1s2) {
                    if (plus->getOper() == opPlus && s1s2->isIntConst()) {
                        query->setSubExp2(new Binary(opPlus,
                                                     query->getSubExp2(),
                                                     new Unary(opNeg, s1s2->clone())));
                        query->setSubExp1(((Binary*)plus)->getSubExp1());
                        change = true;
                    }
                    if (plus->getOper() == opMinus && s1s2->isIntConst()) {
                        query->setSubExp2(new Binary(opPlus, query->getSubExp2(), s1s2->clone()));
                        query->setSubExp1(((Binary*)plus)->getSubExp1());
                        change = true;
                    }
                }
            }


            // substitute using a statement that has the same left as the query
            if (!change && query->getSubExp1()->getOper() == opSubscript) {
                RefExp *r = (RefExp*)query->getSubExp1();
                Statement *s = r->getDef();
                CallStatement *call = dynamic_cast<CallStatement*>(s);
                if (call) {
                    // See if we can prove something about this register.
                    UserProc* destProc = (UserProc*)call->getDestProc();
                    Exp* base = r->getSubExp1();
                    if (destProc && !destProc->isLib() && ((UserProc*)destProc)->cycleGrp != nullptr &&
                            ((UserProc*)destProc)->cycleGrp->find(this) != ((UserProc*)destProc)->cycleGrp->end()) {
                        // The destination procedure may not have preservation proved as yet, because it is involved
                        // in our recursion group. Use the conditional preservation logic to determine whether query is
                        // true for this procedure
                        Exp* provenTo = destProc->getProven(base);
                        if (provenTo) {
                            // There is a proven preservation. Use it to bypass the call
                            Exp* queryLeft = call->localiseExp(provenTo->clone());
                            query->setSubExp1(queryLeft);
                            // Now try everything on the result
                            return prover(query, lastPhis, cache, original, lastPhi);
                        } else {
                            // Check if the required preservation is one of the premises already assumed
                            Exp* premisedTo = destProc->getPremised(base);
                            if (premisedTo) {
                                if (DEBUG_PROOF)
                                    LOG << "conditional preservation for call from " << getName() << " to " <<
                                           destProc->getName() << ", allows bypassing\n";
                                Exp* queryLeft = call->localiseExp(premisedTo->clone());
                                query->setSubExp1(queryLeft);
                                return prover(query, lastPhis, cache, original, lastPhi);
                            } else {
                                // There is no proof, and it's not one of the premises. It may yet succeed, by making
                                // another premise! Example: try to prove esp, depends on whether ebp is preserved, so
                                // recurse to check ebp's preservation. Won't infinitely loop because of the premise map
                                // FIXME: what if it needs a rx = rx + K preservation?
                                Exp* newQuery = new Binary(opEquals,
                                                           base->clone(),
                                                           base->clone());
                                destProc->setPremise(base);
                                if (DEBUG_PROOF)
                                    LOG << "new required premise " << newQuery << " for " << destProc->getName() <<
                                           "\n";
                                // Pass conditional as true, since even if proven, this is conditional on other things
                                bool result = destProc->prove(newQuery, true);
                                destProc->killPremise(base);
                                if (result) {
                                    if (DEBUG_PROOF)
                                        LOG << "conditional preservation with new premise " << newQuery <<
                                               " succeeds for " << destProc->getName() << "\n";
                                    // Use the new conditionally proven result
                                    Exp* queryLeft = call->localiseExp(base->clone());
                                    query->setSubExp1(queryLeft);
                                    return destProc->prover(query, lastPhis, cache, original, lastPhi);
                                } else {
                                    if (DEBUG_PROOF)
                                        LOG << "conditional preservation required premise " << newQuery << " fails!\n";
                                    // Do nothing else; the outer proof will likely fail
                                }
                            }
                        }

                    } // End call involved in this recursion group
                    // Seems reasonable that recursive procs need protection from call loops too
                    Exp *right = call->getProven(r->getSubExp1());    // getProven returns the right side of what is
                    if (right) {                                    //    proven about r (the LHS of query)
                        right = right->clone();
                        if (called.find(call) != called.end() && *called[call] == *query) {
                            LOG << "found call loop to " << call->getDestProc()->getName() << " " << query << "\n";
                            query = new Terminal(opFalse);
                            change = true;
                        } else {
                            called[call] = query->clone();
                            if (DEBUG_PROOF)
                                LOG << "using proven for " << call->getDestProc()->getName() << " "
                                    << r->getSubExp1() << " = " << right << "\n";
                            right = call->localiseExp(right);
                            if (DEBUG_PROOF)
                                LOG << "right with subs: " << right << "\n";
                            query->setSubExp1(right);                // Replace LHS of query with right
                            change = true;
                        }
                    }
                } else if (s && s->isPhi()) {
                    // for a phi, we have to prove the query for every statement
                    PhiAssign *pa = (PhiAssign*)s;
                    PhiAssign::iterator it;
                    bool ok = true;
                    if (lastPhis.find(pa) != lastPhis.end() || pa == lastPhi) {
                        if (DEBUG_PROOF)
                            LOG << "phi loop detected ";
                        ok = (*query->getSubExp2() == *phiInd);
                        if (ok && DEBUG_PROOF)
                            LOG << "(set true due to induction)\n";        // FIXME: induction??!
                        if (!ok && DEBUG_PROOF)
                            LOG << "(set false " << query->getSubExp2() << " != " << phiInd << ")\n";
                    } else {
                        if (DEBUG_PROOF)
                            LOG << "found " << s << " prove for each\n";
                        for (it = pa->begin(); it != pa->end(); it++) {
                            Exp *e = query->clone();
                            RefExp *r1 = (RefExp*)e->getSubExp1();
                            r1->setDef(it->def);
                            if (DEBUG_PROOF)
                                LOG << "proving for " << e << "\n";
                            lastPhis.insert(lastPhi);
                            if (!prover(e, lastPhis, cache, original, pa)) {
                                ok = false;
                                //delete e;
                                break;
                            }
                            lastPhis.erase(lastPhi);
                            //delete e;
                        }
                        if (ok)
                            cache[pa] = query->getSubExp2()->clone();
                    }
                    if (ok)
                        query = new Terminal(opTrue);
                    else
                        query = new Terminal(opFalse);
                    change = true;
                } else if (s && s->isAssign()) {
                    if (s && refsTo.find(s) != refsTo.end()) {
                        LOG << "detected ref loop " << s << "\n";
                        LOG << "refsTo: ";
                        std::set<Statement*>::iterator ll;
                        for (ll = refsTo.begin(); ll != refsTo.end(); ++ll)
                            LOG << (*ll)->getNumber() << ", ";
                        LOG << "\n";
                        assert(false);
                    } else {
                        refsTo.insert(s);
                        query->setSubExp1(((Assign*)s)->getRight()->clone());
                        change = true;
                    }
                }
            }

            // remove memofs from both sides if possible
            if (!change && query->getSubExp1()->getOper() == opMemOf && query->getSubExp2()->getOper() == opMemOf) {
                query->setSubExp1(((Unary*)query->getSubExp1())->getSubExp1());
                query->setSubExp2(((Unary*)query->getSubExp2())->getSubExp1());
                change = true;
            }

            // is ok if both of the memofs are subscripted with nullptr
            if (!change && query->getSubExp1()->getOper() == opSubscript &&
                    query->getSubExp1()->getSubExp1()->getOper() == opMemOf &&
                    ((RefExp*)query->getSubExp1())->getDef() == nullptr &&
                    query->getSubExp2()->getOper() == opSubscript &&
                    query->getSubExp2()->getSubExp1()->getOper() == opMemOf &&
                    ((RefExp*)query->getSubExp2())->getDef() == nullptr) {
                query->setSubExp1(((Unary*)query->getSubExp1()->getSubExp1())->getSubExp1());
                query->setSubExp2(((Unary*)query->getSubExp2()->getSubExp1())->getSubExp1());
                change = true;
            }

            // find a memory def for the right if there is a memof on the left
            // FIXME: this seems pretty much like a bad hack!
            if (!change && query->getSubExp1()->getOper() == opMemOf) {
                StatementList stmts;
                getStatements(stmts);
                StatementList::iterator it;
                for (it = stmts.begin(); it != stmts.end(); it++) {
                    Assign* s = (Assign*)*it;
                    if (s->isAssign() && *s->getRight() == *query->getSubExp2() && s->getLeft()->getOper() == opMemOf) {
                        query->setSubExp2(s->getLeft()->clone());
                        change = true;
                        break;
                    }
                }
            }

            // last chance, swap left and right if haven't swapped before
            if (!change && !swapped) {
                Exp *e = query->getSubExp1();
                query->setSubExp1(query->getSubExp2());
                query->setSubExp2(e);
                change = true;
                swapped = true;
                refsTo.clear();
            }
        } else if (query->isIntConst()) {
            Const *c = (Const*)query;
            query = new Terminal(c->getInt() ? opTrue : opFalse);
        }

        Exp *old = query->clone();

        query = query->clone()->simplify();

        if (change && !(*old == *query) && DEBUG_PROOF) {
            LOG << old << "\n";
        }
        //delete old;
    }

    return query->getOper() == opTrue;
}

// Get the set of locations defined by this proc. In other words, the define set, currently called returns
void UserProc::getDefinitions(LocationSet& ls) {
    int n = signature->getNumReturns();
    for (int j=0; j < n; j++) {
        ls.insert(signature->getReturnExp(j));
    }
}

void Proc::addCallers(std::set<UserProc*>& callers) {
    std::set<CallStatement*>::iterator it;
    for (it = callerSet.begin(); it != callerSet.end(); it++) {
        UserProc* callerProc = (*it)->getProc();
        callers.insert(callerProc);
    }
}
/**
 * Add to a set of callee Procs.
 */
void UserProc::addCallees(std::list<UserProc*>& callees) {
    // SLOW SLOW SLOW
    // this function is evil now... REALLY evil... hope it doesn't get called too often
    std::list<Proc*>::iterator it;
    for (it = calleeList.begin(); it != calleeList.end(); it++) {
        UserProc* callee = (UserProc*)(*it);
        if (callee->isLib())
            continue;
        addCallee(callee);
    }
}

void UserProc::conTypeAnalysis() {
    if (DEBUG_TA)
        LOG << "type analysis for procedure " << getName() << "\n";
    Constraints consObj;
    LocationSet cons;
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator ss;
    // For each statement this proc
    int conscript = 0;
    for (ss = stmts.begin(); ss != stmts.end(); ss++) {
        cons.clear();
        // So we can co-erce constants:
        conscript = (*ss)->setConscripts(conscript);
        (*ss)->genConstraints(cons);
        consObj.addConstraints(cons);
        if (DEBUG_TA)
            LOG << (*ss) << "\n" << &cons << "\n";
        // Remove the sizes immediately the constraints are generated.
        // Otherwise, x and x*8* look like different expressions
        (*ss)->stripSizes();
    }

    std::list<ConstraintMap> solns;
    bool ret = consObj.solve(solns);
    if (VERBOSE || DEBUG_TA) {
        if (!ret)
            LOG << "** could not solve type constraints for proc " << getName() << "!\n";
        else if (solns.size() > 1)
            LOG << "** " << solns.size() << " solutions to type constraints for proc " << getName() << "!\n";
    }

    std::list<ConstraintMap>::iterator it;
    int solnNum = 0;
    ConstraintMap::iterator cc;
    if (DEBUG_TA) {
        for (it = solns.begin(); it != solns.end(); it++) {
            LOG << "solution " << ++solnNum << " for proc " << getName() << "\n";
            ConstraintMap& cm = *it;
            for (cc = cm.begin(); cc != cm.end(); cc++)
                LOG << cc->first << " = " << cc->second << "\n";
            LOG << "\n";
        }
    }

    // Just use the first solution, if there is one
    Prog* prog = getProg();
    if (solns.size()) {
        ConstraintMap& cm = *solns.begin();
        for (cc = cm.begin(); cc != cm.end(); cc++) {
            // Ick. A workaround for now (see test/pentium/sumarray-O4)
            //assert(cc->first->isTypeOf());
            if (!cc->first->isTypeOf()) continue;
            Exp* loc = ((Unary*)cc->first)->getSubExp1();
            assert(cc->second->isTypeVal());
            Type* ty = ((TypeVal*)cc->second)->getType();
            if (loc->isSubscript())
                loc = ((RefExp*)loc)->getSubExp1();
            if (loc->isGlobal()) {
                const char* nam = ((Const*)((Unary*)loc)->getSubExp1())->getStr();
                if (!ty->resolvesToVoid())
                    prog->setGlobalType(nam, ty->clone());
            } else if (loc->isLocal()) {
                const char* nam = ((Const*)((Unary*)loc)->getSubExp1())->getStr();
                setLocalType(nam, ty);
            } else if (loc->isIntConst()) {
                Const* con = (Const*)loc;
                int val = con->getInt();
                if (ty->isFloat()) {
                    // Need heavy duty cast here
                    // MVE: check this! Especially when a double prec float
                    con->setFlt(*(float*)&val);
                    con->setOper(opFltConst);
                } else if (ty->isCString()) {
                    // Convert to a string
                    const char* str = prog->getStringConstant(ADDRESS::g(val), true);
                    if (str) {
                        // Make a string
                        con->setStr(str);
                        con->setOper(opStrConst);
                    }
                }
                else {
                    if (ty->isInteger() && ty->getSize() && ty->getSize() != STD_SIZE)
                        // Wrap the constant in a TypedExp (for a cast)
                        castConst(con->getConscript(), ty);
                }
            }
        }
    }

    // Clear the conscripts. These confuse the fromSSA logic, causing infinite
    // loops
    for (ss = stmts.begin(); ss != stmts.end(); ss++) {
        (*ss)->clearConscripts();
    }
}




bool UserProc::searchAndReplace(Exp *search, Exp *replace) {
    bool ch = false;
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        ch |= s->searchAndReplace(search, replace);
    }
    return ch;
}

unsigned fudge(StatementList::iterator x) {
    StatementList::iterator y = x;
    return *(unsigned*)&y;
}

Exp *UserProc::getProven(Exp *left) {
    // Note: proven information is in the form r28 mapsto (r28 + 4)
    std::map<Exp*, Exp*, lessExpStar>::iterator it = provenTrue.find(left);
    if (it != provenTrue.end())
        return it->second;
    //     not found, try the signature
    // No! The below should only be for library functions!
    // return signature->getProven(left);
    return nullptr;
}

Exp* UserProc::getPremised(Exp* left) {
    std::map<Exp*, Exp*, lessExpStar>::iterator it = recurPremises.find(left);
    if (it != recurPremises.end())
        return it->second;
    return nullptr;
}
//! Return whether e is preserved by this proc
bool UserProc::isPreserved(Exp* e) {
    return provenTrue.find(e) != provenTrue.end() && *provenTrue[e] == *e;
}

/// Cast the constant whose conscript is num to be type ty
void UserProc::castConst(int num, Type* ty) {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        if ((*it)->castConst(num, ty))
            break;
    }
}

/***************************************************************************//**
 *
 * \brief Trim parameters to procedure calls with ellipsis (...). Also add types for ellipsis parameters, if any
 * \returns true if any signature types so added.
 ******************************************************************************/
bool UserProc::ellipsisProcessing() {
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    bool ch = false;
    for (it = cfg->begin(); it != cfg->end(); ++it) {
        CallStatement* c = (CallStatement*) (*it)->getLastStmt(rrit, srit);
        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr || !c->isCall()) continue;
        ch |= c->ellipsisProcessing(prog);
    }
    if (ch)
        fixCallAndPhiRefs();
    return ch;
}

/***************************************************************************//**
 *
 * Before Type Analysis, refs like r28{0} have a nullptr Statement pointer. After this, they will point to an
 * implicit assignment for the location. Thus, during and after type analysis, you can find the type of any
 * location by following the reference to the definition
 * Note: you need something recursive to make sure that child subexpressions are processed before parents
 * Example: m[r28{0} - 12]{0} could end up adding an implicit assignment for r28{-} with a null reference, when other
 * pieces of code add r28{0}
 ******************************************************************************/
void UserProc::addImplicitAssigns() {
    Boomerang::get()->alert_decompile_debug_point(this, "before adding implicit assigns");

    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    ImplicitConverter ic(cfg);
    StmtImplicitConverter sm(&ic, cfg);
    for (it = stmts.begin(); it != stmts.end(); it++) {
        (*it)->accept(&sm);
    }
    cfg->setImplicitsDone();
    df.convertImplicits(cfg);            // Some maps have m[...]{-} need to be m[...]{0} now
    makeSymbolsImplicit();
    // makeParamsImplicit();            // Not necessary yet, since registers are not yet mapped

    Boomerang::get()->alert_decompile_debug_point(this, "after adding implicit assigns");
}

// e is a parameter location, e.g. r8 or m[r28{0}+8]. Lookup a symbol for it
//! Find the implicit definition for \a e and lookup a symbol
const char* UserProc::lookupParam(Exp* e) {
    // Originally e.g. m[esp+K]
    Statement* def = cfg->findTheImplicitAssign(e);
    if (def == nullptr) {
        LOG << "ERROR: no implicit definition for parameter " << e << " !\n";
        return nullptr;
    }
    RefExp* re = new RefExp(e, def);
    Type* ty = def->getTypeFor(e);
    return lookupSym(re, ty);
}
//! Lookup a specific symbol for the given ref
const char* UserProc::lookupSymFromRef(RefExp* r) {
    Statement* def = r->getDef();
    Exp* base = r->getSubExp1();
    Type* ty = def->getTypeFor(base);
    return lookupSym(r, ty);
}
//! Lookup a specific symbol if any, else the general one if any
const char* UserProc::lookupSymFromRefAny(RefExp* r) {
    Statement* def = r->getDef();
    Exp* base = r->getSubExp1();
    Type* ty = def->getTypeFor(base);
    const char* ret = lookupSym(r, ty);
    if (ret)
        return ret;                        // Found a specific symbol
    return lookupSym(base, ty);            // Check for a general symbol
}

const char* UserProc::lookupSym(const Exp* e, Type* ty) {
    if (e->isTypedExp())
        e = ((const TypedExp*)e)->getSubExp1();
    SymbolMap::iterator it;
    it = symbolMap.find(e);
    while (it != symbolMap.end() && *it->first == *e) {
        Exp* sym = it->second;
        assert(sym->isLocal() || sym->isParam());
        const char* name = ((Const*)((Location*)sym)->getSubExp1())->getStr();
        Type* type = getLocalType(name);
        if (type == nullptr) type = getParamType(name);    // Ick currently linear search
        if (type && type->isCompatibleWith(ty))
            return name;
        ++it;
    }
#if 0
    if (e->isSubscript())
        // A subscripted location, where there was no specific mapping. Check for a general mapping covering all
        // versions of the location
        return lookupSym(((RefExp*)e)->getSubExp1(), ty);
#endif
    // Else there is no symbol
    return nullptr;
}
//! Print just the symbol map
void UserProc::printSymbolMap(std::ostream &out, bool html /*= false*/ ) const {
    if (html)
        out << "<br>";
    out << "symbols:\n";
    for (const std::pair<const Exp*,Exp*> &it : symbolMap) {
        const Type* ty = getTypeForLocation(it.second);
        out << "  " << it.first << " maps to " << it.second << " type " << (ty ? ty->getCtype() : "nullptr") << "\n";
        if (html)
            out << "<br>";
    }
    if (html)
        out << "<br>";
    out << "end symbols\n";
}

void UserProc::dumpLocals(std::ostream& os, bool html) const {
    if (html)
        os << "<br>";
    os << "locals:\n";
    for (const std::pair<std::string, Type*> &it : locals) {
        os << it.second->getCtype() << " " << it.first.c_str() << " ";
        const Exp *e = expFromSymbol(it.first.c_str());
        // Beware: for some locals, expFromSymbol() returns nullptr (? No longer?)
        if (e)
            os << e << "\n";
        else
            os << "-\n";
    }
    if (html)
        os << "<br>";
    os << "end locals\n";
}
//! For debugging
void UserProc::dumpSymbolMap() {
    SymbolMap::iterator it;
    for (it = symbolMap.begin(); it != symbolMap.end(); it++) {
        Type* ty = getTypeForLocation(it->second);
        std::cerr << "  " << it->first << " maps to " << it->second << " type " << (ty ? ty->getCtype() : "NULL") <<
                     "\n";
    }
}

//! For debugging
void UserProc::dumpSymbolMapx() {
    SymbolMap::iterator it;
    for (it = symbolMap.begin(); it != symbolMap.end(); it++) {
        Type* ty = getTypeForLocation(it->second);
        std::cerr << "  " << it->first << " maps to " << it->second << " type " << (ty ? ty->getCtype() : "NULL") <<
                     "\n";
        it->first->printx(2);
    }
}

//! For debugging
void UserProc::testSymbolMap() {
    SymbolMap::iterator it1, it2;
    bool OK = true;
    it1 = symbolMap.begin();
    if (it1 != symbolMap.end()) {
        it2 = it1;
        ++it2;
        while (it2 != symbolMap.end()) {
            if (*it2->first < *it1->first) {        // Compare keys
                OK = false;
                std::cerr << "*it2->first < *it1->first: " << it2->first << " < " << it1->first << "!\n";
                // it2->first->printx(0); it1->first->printx(5);
            }
            ++it1;
            ++it2;
        }
    }
    std::cerr << "Symbolmap is " << (OK ? "OK" : "NOT OK!!!!!") << "\n";
}

void UserProc::dumpLocals() {
    std::stringstream ost;
    dumpLocals(ost);
    std::cerr << ost.str();
}
//! Update the arguments in calls
void UserProc::updateArguments() {
    Boomerang::get()->alert_decompiling(this);
    if (VERBOSE)
        LOG << "### update arguments for " << getName() << " ###\n";
    Boomerang::get()->alert_decompile_debug_point(this, "before updating arguments");
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    for (it = cfg->begin(); it != cfg->end(); ++it) {
        CallStatement* c = (CallStatement*) (*it)->getLastStmt(rrit, srit);
        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr || !c->isCall()) continue;
        c->updateArguments();
        //c->bypass();
        if (VERBOSE) {
            std::ostringstream ost;
            c->print(ost);
            LOG << ost.str() << "\n";
        }
    }
    if (VERBOSE)
        LOG << "=== end update arguments for " << getName() << "\n";
    Boomerang::get()->alert_decompile_debug_point(this, "after updating arguments");
}
//! Update the defines in calls
void UserProc::updateCallDefines() {
    if (VERBOSE)
        LOG << "### update call defines for " << getName() << " ###\n";
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        CallStatement* call = dynamic_cast<CallStatement*>(*it);
        if (call == nullptr) continue;
        call->updateDefines();
    }
}
//! Replace simple global constant references
//! Statement level transform :
//! PREDICATE: (statement IS_A Assign) AND (statement.rhs IS_A MemOf) AND (statement.rhs.sub(1) IS_A IntConst)
//! ACTION:
//!		$tmp_addr = assgn.rhs.sub(1);
//!		$tmp_val  = prog->readNative($tmp_addr,statement.type.bitwidth/8);
//!		statement.rhs.replace_with(Const($tmp_val))
void UserProc::replaceSimpleGlobalConstants() {
    if (VERBOSE)
        LOG << "### replace simple global constants for " << getName() << " ###\n";
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (Statement *st : stmts) {
        Assign* assgn = dynamic_cast<Assign*>(st);
        if (assgn == nullptr)
            continue;
        if (!assgn->getRight()->isMemOf())
            continue;
        if (!assgn->getRight()->getSubExp1()->isIntConst())
            continue;
        ADDRESS addr = ((Const*)assgn->getRight()->getSubExp1())->getAddr();
        LOG << "assgn " << assgn << "\n";
        if (prog->isReadOnly(addr)) {
            LOG << "is readonly\n";
            int val;
            switch (assgn->getType()->getSize()) {
                case 8:
                    val = prog->readNative1(addr);
                    break;
                case 16:
                    val = prog->readNative2(addr);
                    break;
                case 32:
                    val = prog->readNative4(addr);
                    break;
                default:
                    assert(false);
            }
            assgn->setRight(new Const(val));
        }
    }
}
void UserProc::reverseStrengthReduction() {
    Boomerang::get()->alert_decompile_debug_point(this, "before reversing strength reduction");

    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++)
        if ((*it)->isAssign()) {
            Assign *as = (Assign*)*it;
            // of the form x = x{p} + c
            if (as->getRight()->getOper() == opPlus &&
                    as->getRight()->getSubExp1()->isSubscript() &&
                    *as->getLeft() == *as->getRight()->getSubExp1()->getSubExp1() &&
                    as->getRight()->getSubExp2()->isIntConst()) {
                int c = ((Const*)as->getRight()->getSubExp2())->getInt();
                RefExp *r = (RefExp*)as->getRight()->getSubExp1();
                if (r->getDef() && r->getDef()->isPhi()) {
                    PhiAssign *p = (PhiAssign*)r->getDef();
                    if (p->getNumDefs() == 2) {
                        Statement *first = p->getDefs().front().def;
                        Statement *second = p->getDefs().back().def;
                        if (first == as) {
                            // want the increment in second
                            Statement *tmp = first;
                            first = second;
                            second = tmp;
                        }
                        // first must be of form x := 0
                        if (first && first->isAssign() &&
                                ((Assign*)first)->getRight()->isIntConst() &&
                                ((Const*)((Assign*)first)->getRight())->getInt() == 0) {
                            // ok, fun, now we need to find every reference to p and
                            // replace with x{p} * c
                            StatementList stmts2;
                            getStatements(stmts2);
                            StatementList::iterator it2;
                            for (it2 = stmts2.begin(); it2 != stmts2.end(); it2++)
                                if (*it2 != as)
                                    (*it2)->searchAndReplace(r, new Binary(opMult, r->clone(), new Const(c)));
                            // that done we can replace c with 1 in as
                            ((Const*)as->getRight()->getSubExp2())->setInt(1);
                        }
                    }
                }
            }
        }
    Boomerang::get()->alert_decompile_debug_point(this, "after reversing strength reduction");
}
/***************************************************************************//**
 *
 * \brief Insert into parameters list correctly sorted
 *
 * Update the parameters, in case the signature and hence ordering and filtering has changed, or the locations in the
 * collector have changed
 *
 ******************************************************************************/
void UserProc::insertParameter(Exp* e, Type* ty) {

    if (filterParams(e))
        return;                        // Filtered out
    // Used to filter out preserved locations here: no! Propagation and dead code elimination solve the problem.
    // See test/pentium/restoredparam for an example where you must not remove restored locations

    // Wrap it in an implicit assignment; DFA based TA should update the type later
    ImplicitAssign* as = new ImplicitAssign(ty->clone(), e->clone());
    // Insert as, in order, into the existing set of parameters
    StatementList::iterator nn;
    bool inserted = false;
    for (nn = parameters.begin(); nn != parameters.end(); ++nn) {
        // If the new assignment is less than the current one ...
        if (signature->argumentCompare(*as, *(Assignment*)*nn)) {
            nn = parameters.insert(nn, as);        // ... then insert before this position
            inserted = true;
            break;
        }
    }
    if (!inserted)
        parameters.insert(parameters.end(), as);    // In case larger than all existing elements

    // update the signature
    signature->setNumParams(0);
    int i = 1;
    for (nn = parameters.begin(); nn != parameters.end(); ++nn, ++i) {
        Assignment *a = (Assignment*)*nn;
        char tmp[20];
        sprintf(tmp, "param%i", i);
        signature->addParameter(a->getType(), tmp, a->getLeft());
    }
}

/***************************************************************************//**
 *
 * \brief Decide whether to filter out \a e (return true) or keep it
 *
 * Filter out locations not possible as return locations. Return true to *remove* (filter *out*)
 *
 * \returns true if \a e  should be filtered out
 ******************************************************************************/
bool UserProc::filterReturns(Exp* e) {
    if (isPreserved(e))
        // If it is preserved, then it can't be a return (since we don't change it)
        return true;
    switch (e->getOper()) {
        case opPC:    return true;            // Ignore %pc
        case opDefineAll: return true;        // Ignore <all>
        case opTemp: return true;            // Ignore all temps (should be local to one instruction)
            // Would like to handle at least %ZF, %CF one day. For now, filter them out
        case opZF: case opCF: case opFlags:
            return true;
        case opMemOf: {
            // return signature->isStackLocal(prog, e);        // Filter out local variables
            // Actually, surely all sensible architectures will only every return in registers. So for now, just
            // filter out all mem-ofs
            return true;
        }
        case opGlobal:
            return true;                // Never return in globals
        default:
            return false;
    }
    return false;
}

//
/***************************************************************************//**
 *
 * \brief Decide whether to filter out \a e (return true) or keep it
 *
 * Filter out locations not possible as parameters or arguments. Return true to remove
 *
 * \returns true if \a e  should be filtered out
 * \sa UserProc::filterReturns
 *
 ******************************************************************************/
bool UserProc::filterParams(Exp* e) {
    switch (e->getOper()) {
        case opPC:    return true;
        case opTemp: return true;
        case opRegOf: {
            int sp = 999;
            if (signature) sp = signature->getStackRegister(prog);
            int r = ((Const*)((Location*)e)->getSubExp1())->getInt();
            return r == sp;
        }
        case opMemOf: {
            Exp* addr = ((Location*)e)->getSubExp1();
            if (addr->isIntConst())
                return true;            // Global memory location
            if (addr->isSubscript() && ((RefExp*)addr)->isImplicitDef()) {
                Exp* reg = ((RefExp*)addr)->getSubExp1();
                int sp = 999;
                if (signature) sp = signature->getStackRegister(prog);
                if (reg->isRegN(sp))
                    return true;        // Filter out m[sp{-}] assuming it is the return address
            }
            return false;                // Might be some weird memory expression that is not a local
        }
        case opGlobal:
            return true;                // Never use globals as argument locations (could appear on RHS of args)
        default:
            return false;
    }
    return false;
}
/// Determine whether e is a local, either as a true opLocal (e.g. generated by fromSSA), or if it is in the
/// symbol map and the name is in the locals map. If it is a local, return its name, else nullptr
const char* UserProc::findLocal(Exp* e, Type* ty) {
    if (e->isLocal())
        return ((Const*)((Unary*)e)->getSubExp1())->getStr();
    // Look it up in the symbol map
    const char* name = lookupSym(e, ty);
    if (name == nullptr)
        return nullptr;
    // Now make sure it is a local; some symbols (e.g. parameters) are in the symbol map but not locals
    std::string str(name);
    if (locals.find(str) != locals.end())
        return name;
    return nullptr;
}

const char* UserProc::findLocalFromRef(RefExp* r) {
    Statement* def = r->getDef();
    Exp* base = r->getSubExp1();
    Type* ty = def->getTypeFor(base);
    const char* name = lookupSym(r, ty);
    if (name == nullptr)
        return nullptr;
    // Now make sure it is a local; some symbols (e.g. parameters) are in the symbol map but not locals
    std::string str(name);
    if (locals.find(str) != locals.end())
        return name;
    return nullptr;
}

const char* UserProc::findFirstSymbol(Exp* e) {
    SymbolMap::iterator ff = symbolMap.find(e);
    if (ff == symbolMap.end()) return nullptr;
    return ((Const*)((Location*)ff->second)->getSubExp1())->getStr();
}


// Algorithm:
/* fixCallAndPhiRefs
        for each statement s in this proc
          if s is a phi statement ps
                let r be a ref made up of lhs and s
                for each parameter p of ps
                  if p == r                        // e.g. test/pentium/fromssa2 r28{56}
                        remove p from ps
                let lhs be left hand side of ps
                allSame = true
                let first be a ref built from first p
                do bypass but not propagation on first
                if result is of the form lhs{x}
                  replace first with x
                for each parameter p of ps after the first
                  let current be a ref built from p
                  do bypass but not propagation on current
                  if result is of form lhs{x}
                        replace cur with x
                  if first != current
                        allSame = false
                if allSame
                  let best be ref built from the "best" parameter p in ps ({-} better than {assign} better than {call})
                  replace ps with an assignment lhs := best
        else (ordinary statement)
          do bypass and propagation for s
*/
//Perform call and phi statement bypassing at depth d <- missing

/***************************************************************************//**
 *
 * \brief  Perform call and phi statement bypassing at all depths
 *
 ******************************************************************************/
void UserProc::fixCallAndPhiRefs() {
    if (VERBOSE)
        LOG << "### start fix call and phi bypass analysis for " << getName() << " ###\n";

    Boomerang::get()->alert_decompile_debug_point(this, "before fixing call and phi refs");

    std::map<Exp*, int, lessExpStar> destCounts;
    StatementList::iterator it;
    Statement* s;
    StatementList stmts;
    getStatements(stmts);

    // a[m[]] hack, aint nothing better.
    bool found = true;
    for (it = stmts.begin(); it != stmts.end(); it++)
        if ((*it)->isCall()) {
            CallStatement *call = (CallStatement*)*it;
            for (StatementList::iterator it1 = call->getArguments().begin(); it1 != call->getArguments().end(); it1++) {
                Assign *a = (Assign*)*it1;
                if (a->getType()->resolvesToPointer()) {
                    Exp *e = a->getRight();
                    if (e->getOper() == opPlus || e->getOper() == opMinus)
                        if (e->getSubExp2()->isIntConst())
                            if (e->getSubExp1()->isSubscript() &&
                                    e->getSubExp1()->getSubExp1()->isRegN(signature->getStackRegister()) &&
                                    (((RefExp*)e->getSubExp1())->getDef() == nullptr ||
                                     ((RefExp*)e->getSubExp1())->getDef()->isImplicit())) {
                                a->setRight(new Unary(opAddrOf, Location::memOf(e->clone())));
                                found = true;
                            }
                }
            }
        }
    if (found)
        doRenameBlockVars(2);

    // Scan for situations like this:
    // 56 r28 := phi{6, 26}
    // ...
    // 26 r28 := r28{56}
    // So we can remove the second parameter, then reduce the phi to an assignment, then propagate it
    for (it = stmts.begin(); it != stmts.end(); it++) {
        s = *it;
        if (s->isPhi()) {
            PhiAssign* ps = (PhiAssign*)s;
            RefExp* r = new RefExp(ps->getLeft(), ps);
            for (PhiAssign::iterator p = ps->begin(); p != ps->end(); ) {
                if (p->e == nullptr) {                        // Can happen due to PhiAssign::setAt
                    ++p;
                    continue;
                }
                Exp* current = new RefExp(p->e, p->def);
                if (*current == *r) {                    // Will we ever see this?
                    p = ps->erase(p);                    // Erase this phi parameter
                    continue;
                }
                // Chase the definition
                if (p->def) {
                    if (!p->def->isAssign()) {
                        ++p;
                        continue;
                    }
                    Exp* rhs = ((Assign*)p->def)->getRight();
                    if (*rhs == *r) {                    // Check if RHS is a single reference to ps
                        p = ps->erase(p);                // Yes, erase this phi parameter
                        continue;
                    }
                }
                ++p;
            }
        }
    }

    // Second pass
    for (it = stmts.begin(); it != stmts.end(); it++) {
        s = *it;
        if (s->isPhi()) {
            PhiAssign* ps = (PhiAssign*)s;
            if (ps->getNumDefs() == 0) continue;        // Can happen e.g. for m[...] := phi {} when this proc is
            // involved in a recursion group
            Exp* lhs = ps->getLeft();
            bool allSame = true;
            // Let first be a reference built from the first parameter
            PhiAssign::iterator p = ps->begin();
            while (p->e == nullptr && p != ps->end())
                ++p;                                    // Skip any null parameters
            assert(p != ps->end());                        // Should have been deleted
            Exp* first = new RefExp(p->e, p->def);
            // bypass to first
            CallBypasser cb(ps);
            first = first->accept(&cb);
            if (cb.isTopChanged())
                first = first->simplify();
            first = first->propagateAll();                // Propagate everything repeatedly
            if (cb.isMod()) {                        // Modified?
                // if first is of the form lhs{x}
                if (first->isSubscript() && *((RefExp*)first)->getSubExp1() == *lhs)
                    // replace first with x
                    p->def = ((RefExp*)first)->getDef();
            }
            // For each parameter p of ps after the first
            for (++p; p != ps->end(); ++p) {
                if (p->e == nullptr) continue;
                Exp* current = new RefExp(p->e, p->def);
                CallBypasser cb2(ps);
                current = current->accept(&cb2);
                if (cb2.isTopChanged())
                    current = current->simplify();
                current = current->propagateAll();
                if (cb2.isMod()    )                    // Modified?
                    // if current is of the form lhs{x}
                    if (current->isSubscript() && *((RefExp*)current)->getSubExp1() == *lhs)
                        // replace current with x
                        p->def = ((RefExp*)current)->getDef();
                if (!(*first == *current))
                    allSame = false;
            }

            if (allSame) {
                // let best be ref built from the "best" parameter p in ps ({-} better than {assign} better than {call})
                p = ps->begin();
                while (p->e == nullptr && p != ps->end())
                    ++p;                                    // Skip any null parameters
                assert(p != ps->end());                        // Should have been deleted
                RefExp* best = new RefExp(p->e, p->def);
                for (++p; p != ps->end(); ++p) {
                    if (p->e == nullptr) continue;
                    RefExp* current = new RefExp(p->e, p->def);
                    if (current->isImplicitDef()) {
                        best = current;
                        break;
                    }
                    if (p->def->isAssign())
                        best = current;
                    // If p->def is a call, this is the worst case; keep only (via first) if all parameters are calls
                }
                ps->convertToAssign(best);
                if (VERBOSE)
                    LOG << "redundant phi replaced with copy assign; now " << ps << "\n";
            }
        } else {    // Ordinary statement
            s->bypass();
        }
    }

    // Also do xxx in m[xxx] in the use collector
    UseCollector::iterator cc;
    for (cc = col.begin(); cc != col.end(); ++cc) {
        if (!(*cc)->isMemOf()) continue;
        Exp* addr = ((Location*)*cc)->getSubExp1();
        CallBypasser cb(nullptr);
        addr = addr->accept(&cb);
        if (cb.isMod())
            ((Location*)*cc)->setSubExp1(addr);
    }

    if (VERBOSE)
        LOG << "### end fix call and phi bypass analysis for " << getName() << " ###\n";

    Boomerang::get()->alert_decompile_debug_point(this, "after fixing call and phi refs");
}

// Not sure that this is needed...
/***************************************************************************//**
 *
 * \brief Mark calls involved in the recursion cycle as non childless
 * (each child has had middleDecompile called on it now).
 *
 ******************************************************************************/
void UserProc::markAsNonChildless(ProcSet* cs) {
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    BB_IT it;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
        CallStatement* c = (CallStatement*) bb->getLastStmt(rrit, srit);
        if (c && c->isCall() && c->isChildless()) {
            UserProc* dest = (UserProc*)c->getDestProc();
            if (cs->find(dest) != cs->end())     // Part of the cycle?
                // Yes, set the callee return statement (making it non childless)
                c->setCalleeReturn(dest->getTheReturnStatement());
        }
    }
}

// Propagate into xxx of m[xxx] in the UseCollector (locations live at the entry of this proc)
void UserProc::propagateToCollector() {
    UseCollector::iterator it;
    for (it = col.begin(); it != col.end(); ) {
        if (!(*it)->isMemOf()) {
            ++it;
            continue;
        }
        Exp* addr = ((Location*)*it)->getSubExp1();
        LocationSet used;
        LocationSet::iterator uu;
        addr->addUsedLocs(used);
        for (uu = used.begin(); uu != used.end(); uu++) {
            RefExp* r = (RefExp*)*uu;
            if (!r->isSubscript()) continue;
            Assign* as = (Assign*)r->getDef();
            if (as == nullptr || !as->isAssign()) continue;
            bool ch;
            Exp* res = addr->clone()->searchReplaceAll(r, as->getRight(), ch);
            if (!ch) continue;                // No change
            Exp* memOfRes = Location::memOf(res)->simplify();
            // First check to see if memOfRes is already in the set
            if (col.exists(memOfRes)) {
                // Take care not to use an iterator to the newly erased element.
                /* it = */ col.remove(it++);        // Already exists; just remove the old one
                continue;
            } else {
                if (VERBOSE)
                    LOG << "propagating " << r << " to " << as->getRight() << " in collector; result " << memOfRes <<
                           "\n";
                ((Location*)*it)->setSubExp1(res);    // Change the child of the memof
            }
        }
        ++it;            // it is iterated either with the erase, or the continue, or here
    }
}

/***************************************************************************//**
 *
 * \brief Get the initial parameters, based on this UserProc's use collector
 * Probably unused now
 *
 ******************************************************************************/
void UserProc::initialParameters() {
    if (VERBOSE)
        LOG << "### initial parameters for " << getName() << "\n";
    parameters.clear();
    UseCollector::iterator cc;
    for (cc = col.begin(); cc != col.end(); ++cc)
        parameters.append(new ImplicitAssign((*cc)->clone()));
    if (VERBOSE) {
        std::ostringstream ost;
        printParams(ost);
        LOG << ost.str();
    }
}

/***************************************************************************//**
 *
 * \brief The inductive preservation analysis.
 *
 ******************************************************************************/
bool UserProc::inductivePreservation(UserProc* topOfCycle) {
    // FIXME: This is not correct in general!! It should work OK for self recursion, but not for general mutual
    //recursion. Not that hard, just not done yet.
    return true;
}
//! True if e represents a stack local variable
bool UserProc::isLocal(Exp* e) {
    if (!e->isMemOf())
        return false;            // Don't want say a register
    SymbolMap::iterator ff = symbolMap.find(e);
    if (ff == symbolMap.end())
        return false;
    Exp* mapTo = ff->second;
    return mapTo->isLocal();
}
//! True if e can be propagated
bool UserProc::isPropagatable(Exp* e) {
    if (addressEscapedVars.exists(e)) return false;
    return isLocalOrParam(e);
}
//! True if e represents a stack local or stack param
bool UserProc::isLocalOrParam(Exp* e) {
    if (isLocal(e)) return true;
    return parameters.existsOnLeft(e);
}

// Is this m[sp{-} +/- K]?
//! True if e could represent a stack local or stack param
bool UserProc::isLocalOrParamPattern(Exp* e) {
    if (!e->isMemOf())
        return false;            // Don't want say a register
    Exp* addr = ((Location*)e)->getSubExp1();
    if (!signature->isPromoted())
        return false;    // Prevent an assert failure if using -E
    int sp = signature->getStackRegister();
    Exp* initSp = new RefExp(Location::regOf(sp), nullptr);    // sp{-}
    if (*addr == *initSp)
        return true;            // Accept m[sp{-}]
    if (addr->getArity() != 2)
        return false;    // Require sp +/- K
    OPER op = ((Binary*)addr)->getOper();
    if (op != opPlus && op != opMinus)
        return false;
    Exp* left =  ((Binary*)addr)->getSubExp1();
    if (!(*left == *initSp))
        return false;
    Exp* right = ((Binary*)addr)->getSubExp2();
    return right->isIntConst();
}


//
//
/***************************************************************************//**
 *
 * \brief Used for checking for unused parameters
 * \param visited - a set of procs already visited, to prevent infinite recursion
 *
 * Remove the unused parameters. Check for uses for each parameter as param{0}.
 * Some parameters are apparently used when in fact they are only used as parameters to calls to procedures in the
 * recursion set. So don't count components of arguments of calls in the current recursion group that chain through to
 * ultimately use the argument as a parameter to the current procedure.
 * Some parameters are apparently used when in fact they are only used by phi statements which transmit a return from
 * a recursive call ultimately to the current procedure, to the exit of the current procedure, and the return exists
 * only because of a liveness created by a parameter to a recursive call. So when examining phi statements, check if
 * referenced from a return of the current procedure, and has an implicit operand, and all the others satisfy a call
 * to doesReturnChainToCall(param, this proc).
 * but not including removing unused statements.
 *
 * \returns true/false :P
 *
 ******************************************************************************/
bool UserProc::doesParamChainToCall(Exp* param, UserProc* p, ProcSet* visited) {
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    for (it = cfg->begin(); it != cfg->end(); ++it) {
        CallStatement* c = (CallStatement*) (*it)->getLastStmt(rrit, srit);
        if (c == nullptr || !c->isCall())  continue;        // Only interested in calls
        UserProc* dest = (UserProc*)c->getDestProc();
        if (dest == nullptr || dest->isLib()) continue;  // Only interested in calls to UserProcs
        if (dest == p) {                // Pointer comparison is OK here
            // This is a recursive call to p. Check for an argument of the form param{-} FIXME: should be looking for
            // component
            StatementList& args = c->getArguments();
            StatementList::iterator aa;
            for (aa = args.begin(); aa != args.end(); ++aa) {
                Exp* rhs = ((Assign*)*aa)->getRight();
                if (rhs && rhs->isSubscript() && ((RefExp*)rhs)->isImplicitDef()) {
                    Exp* base = ((RefExp*)rhs)->getSubExp1();
                    // Check if this argument location matches loc
                    if (*base == *param)
                        // We have a call to p that takes param{-} as an argument
                        return true;
                }
            }
        } else {
            if (dest->doesRecurseTo(p)) {
                // We have come to a call that is not to p, but is in the same recursion group as p and this proc.
                visited->insert(this);
                if (visited->find(dest) != visited->end()) {
                    // Recurse to the next proc
                    bool res = dest->doesParamChainToCall(param, p, visited);
                    if (res)
                        return true;
                    //TODO: Else consider more calls this proc
                }
            }
        }
    }
    return false;
}

bool UserProc::isRetNonFakeUsed(CallStatement* c, Exp* retLoc, UserProc* p, ProcSet* visited) {
    // Ick! This algorithm has to search every statement for uses of the return location retLoc defined at call c that
    // are not arguments of calls to p. If we had def-use information, it would be much more efficient
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        LocationSet ls;
        LocationSet::iterator ll;
        s->addUsedLocs(ls);
        bool found = false;
        for (ll = ls.begin(); ll != ls.end(); ++ll) {
            if (!(*ll)->isSubscript()) continue;
            Statement* def = ((RefExp*)*ll)->getDef();
            if (def != c) continue;                            // Not defined at c, ignore
            Exp* base = ((RefExp*)*ll)->getSubExp1();
            if (!(*base == *retLoc)) continue;                // Defined at c, but not the right location
            found = true;
            break;
        }
        if (!found)
            continue;
        if (!s->isCall())
            // This non-call uses the return; return true as it is non-fake used
            return true;
        UserProc* dest = (UserProc*)((CallStatement*)s)->getDestProc();
        if (dest == nullptr)
            // This childless call seems to use the return. Count it as a non-fake use
            return true;
        if (dest == p)
            // This procedure uses the parameter, but it's a recursive call to p, so ignore it
            continue;
        if (dest->isLib())
            // Can't be a recursive call
            return true;
        if (!dest->doesRecurseTo(p))
            return true;
        // We have a call that uses the return, but it may well recurse to p
        visited->insert(this);
        if (visited->find(dest) != visited->end())
            // We've not found any way for loc to be fake-used. Count it as non-fake
            return true;
        if (!doesParamChainToCall(retLoc, p, visited))
            // It is a recursive call, but it doesn't end up passing param as an argument in a call to p
            return true;
    }
    return false;
}

// Check for a gainful use of bparam{0} in this proc. Return with true when the first such use is found.
// Ignore uses in return statements of recursive functions, and phi statements that define them
// Procs in visited are already visited
///         Reurn true if location e is used gainfully in this procedure. visited is a set of UserProcs already
///            visited.
bool UserProc::checkForGainfulUse(Exp* bparam, ProcSet& visited) {
    visited.insert(this);                    // Prevent infinite recursion
    StatementList::iterator pp;
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        // Special checking for recursive calls
        if (s->isCall()) {
            CallStatement* c = (CallStatement*)s;
            UserProc* dest = (UserProc*)c->getDestProc();
            if (dest && !dest->isLib() && dest->doesRecurseTo(this)) {
                // In the destination expression?
                LocationSet u;
                c->getDest()->addUsedLocs(u);
                if (u.existsImplicit(bparam))
                    return true;            // Used by the destination expression
                // Else check for arguments of the form lloc := f(bparam{0})
                StatementList& args = c->getArguments();
                StatementList::iterator aa;
                for (aa = args.begin(); aa != args.end(); ++aa) {
                    Exp* rhs = ((Assign*)*aa)->getRight();
                    LocationSet argUses;
                    rhs->addUsedLocs(argUses);
                    if (argUses.existsImplicit(bparam)) {
                        Exp* lloc = ((Assign*)*aa)->getLeft();
                        if (visited.find(dest) == visited.end() && dest->checkForGainfulUse(lloc, visited))
                            return true;
                    }
                }
                // If get to here, then none of the arguments is of this form, and we can ignore this call
                continue;
            }
        }
        else if (s->isReturn()) {
            if (cycleGrp && cycleGrp->size())        // If this function is involved in recursion
                continue;                //  then ignore this return statement
        } else if (s->isPhi() && theReturnStatement != nullptr && cycleGrp && cycleGrp->size()) {
            Exp* phiLeft = ((PhiAssign*)s)->getLeft();
            RefExp* refPhi = new RefExp(phiLeft, s);
            ReturnStatement::iterator rr;
            bool foundPhi = false;
            for (rr = theReturnStatement->begin(); rr != theReturnStatement->end(); ++rr) {
                Exp* rhs = ((Assign*)*rr)->getRight();
                LocationSet uses;
                rhs->addUsedLocs(uses);
                if (uses.exists(refPhi)) {
                    // s is a phi that defines a component of a recursive return. Ignore it
                    foundPhi = true;
                    break;
                }
            }
            if (foundPhi)
                continue;            // Ignore this phi
        }

        // Otherwise, consider uses in s
        LocationSet uses;
        s->addUsedLocs(uses);
        if (uses.existsImplicit(bparam))
            return true;                // A gainful use
    }    // for each statement s

    return false;
}

// See comments three procedures above
//! Remove redundant parameters. Return true if remove any
bool UserProc::removeRedundantParameters() {
    if (signature->isForced())
        // Assume that no extra parameters would have been inserted... not sure always valid
        return false;

    bool ret = false;
    StatementList newParameters;

    Boomerang::get()->alert_decompile_debug_point(this, "before removing redundant parameters");

    if (DEBUG_UNUSED)
        LOG << "%%% removing unused parameters for " << getName() << "\n";
    // Note: this would be far more efficient if we had def-use information
    StatementList::iterator pp;
    for (pp = parameters.begin(); pp != parameters.end(); ++pp) {
        Exp* param = ((Assign*)*pp)->getLeft();
        bool az;
        Exp* bparam = param->clone()->removeSubscripts(az);        // FIXME: why does main have subscripts on parameters?
        // Memory parameters will be of the form m[sp + K]; convert to m[sp{0} + K] as will be found in uses
        bparam = bparam->expSubscriptAllNull();        // Now m[sp{-}+K]{-}
        ImplicitConverter ic(cfg);
        bparam = bparam->accept(&ic);                // Now m[sp{0}+K]{0}
        assert(bparam->isSubscript());
        bparam = ((RefExp*)bparam)->getSubExp1();    // now m[sp{0}+K] (bare parameter)

        ProcSet visited;
        if (checkForGainfulUse(bparam, visited))
            newParameters.append(*pp);                // Keep this parameter
        else {
            // Remove the parameter
            ret = true;
            if (DEBUG_UNUSED)
                LOG << " %%% removing unused parameter " << param << " in " << getName() << "\n";
            // Check if it is in the symbol map. If so, delete it; a local will be created later
            SymbolMap::iterator ss = symbolMap.find(param);
            if (ss != symbolMap.end())
                symbolMap.erase(ss);            // Kill the symbol
            signature->removeParameter(param);    // Also remove from the signature
            cfg->removeImplicitAssign(param);    // Remove the implicit assignment so it doesn't come back
        }
    }
    parameters = newParameters;
    if (DEBUG_UNUSED)
        LOG << "%%% end removing unused parameters for " << getName() << "\n";

    Boomerang::get()->alert_decompile_debug_point(this, "after removing redundant parameters");

    return ret;
}

/***************************************************************************//**
 *
 * \brief Remove any returns that are not used by any callers
 *
 * Remove unused returns for this procedure, based on the equation:
 * returns = modifieds isect union(live at c) for all c calling this procedure.
 * The intersection operation will only remove locations. Removing returns can have three effects for each component y
 * used by that return (e.g. if return r24 := r25{10} + r26{20} is removed, statements 10 and 20 will be affected and
 * y will take the values r25{10} and r26{20}):
 * 1) a statement s defining a return becomes unused if the only use of its definition was y
 * 2) a call statement c defining y will no longer have y live if the return was the only use of y. This could cause a
 *    change to the returns of c's destination, so removeRedundantReturns has to be called for c's destination proc (if it
 *    turns out to be the only definition, and that proc was not already scheduled for return removing).
 * 3) if y is a parameter (i.e. y is of the form loc{0}), then the signature of this procedure changes, and all callers
 *    have to have their arguments trimmed, and a similar process has to be applied to all those caller's removed
 *    arguments as is applied here to the removed returns.
 * The \a removeRetSet is the set of procedures to process with this logic; caller in Prog calls all elements in this set
 * (only add procs to this set, never remove)
 *
 * \returns true if any change
 ******************************************************************************/

bool UserProc::removeRedundantReturns(std::set<UserProc*>& removeRetSet) {
    Boomerang::get()->alert_decompiling(this);
    Boomerang::get()->alert_decompile_debug_point(this, "before removing unused returns");
    // First remove the unused parameters
    bool removedParams = removeRedundantParameters();
    if (theReturnStatement == nullptr)
        return removedParams;
    if (DEBUG_UNUSED)
        LOG << "%%% removing unused returns for " << getName() << " %%%\n";

    if (signature->isForced()) {
        // Respect the forced signature, but use it to remove returns if necessary
        bool removedRets = false;
        ReturnStatement::iterator rr;
        for (rr = theReturnStatement->begin(); rr != theReturnStatement->end(); ) {
            Assign* a = (Assign*)*rr;
            Exp *lhs = a->getLeft();
            // For each location in the returns, check if in the signature
            bool found = false;
            for (unsigned int i = 0; i < signature->getNumReturns(); i++)
                if (*signature->getReturnExp(i) == *lhs) {
                    found = true;
                    break;
                }
            if (found)
                rr++;        // Yes, in signature; OK
            else {
                // This return is not in the signature. Remove it
                rr = theReturnStatement->erase(rr);
                removedRets = true;
                if (DEBUG_UNUSED)
                    LOG << "%%%  removing unused return " << a << " from proc " << getName() << " (forced signature)\n";
            }
        }
        if (removedRets)
            // Still may have effects on calls or now unused statements
            updateForUseChange(removeRetSet);
        return removedRets;
    }

    // FIXME: this needs to be more sensible when we don't decompile down from main! Probably should assume just the
    // first return is valid, for example (presently assume none are valid)
    LocationSet unionOfCallerLiveLocs;
    if (strcmp(getName(), "main") == 0)        // Probably not needed: main is forced so handled above
        // Just insert one return for main. Note: at present, the first parameter is still the stack pointer
        unionOfCallerLiveLocs.insert(signature->getReturnExp(1));
    else {
        // For each caller
        std::set<CallStatement*>& callers = getCallers();
        std::set<CallStatement*>::iterator cc;
        for (cc = callers.begin(); cc != callers.end(); ++cc) {
            // Union in the set of locations live at this call
            UseCollector* useCol = (*cc)->getUseCollector();
            unionOfCallerLiveLocs.makeUnion(useCol->getLocSet());
        }
    }
    // Intersect with the current returns
    bool removedRets = false;
    ReturnStatement::iterator rr;
    for (rr = theReturnStatement->begin(); rr != theReturnStatement->end(); ) {
        Assign* a = (Assign*)*rr;
        if (!unionOfCallerLiveLocs.exists(a->getLeft())) {
            if (DEBUG_UNUSED)
                LOG << "%%%  removing unused return " << a << " from proc " << getName() << "\n";
            // If a component of the RHS referenced a call statement, the liveness used to be killed here.
            // This was wrong; you need to notice the liveness changing inside updateForUseChange() to correctly
            // recurse to callee
            rr = theReturnStatement->erase(rr);
            removedRets = true;
        }
        else
            rr++;
    }

    if (DEBUG_UNUSED) {
        std::ostringstream ost;
        unionOfCallerLiveLocs.print(ost);
        LOG << "%%%  union of caller live locations for " << getName() << ": " << ost.str() << "\n";
        LOG << "%%%  final returns for " << getName() << ": " << theReturnStatement->getReturns().prints() << "\n";
    }

    // removing returns might result in params that can be removed, might as well do it now.
    removedParams |= removeRedundantParameters();

    ProcSet updateSet;            // Set of procs to update

    if (removedParams || removedRets) {
        // Update the statements that call us
        std::set<CallStatement*>::iterator it;
        for (it = callerSet.begin(); it != callerSet.end() ; it++) {
            (*it)->updateArguments();                // Update caller's arguments
            updateSet.insert((*it)->getProc());        // Make sure we redo the dataflow
            removeRetSet.insert((*it)->getProc());    // Also schedule caller proc for more analysis
        }

        // Now update myself
        updateForUseChange(removeRetSet);

        // Update any other procs that need updating
        updateSet.erase(this);        // Already done this proc
        while (updateSet.size()) {
            UserProc* proc = *updateSet.begin();
            updateSet.erase(proc);
            proc->updateForUseChange(removeRetSet);
        }
    }

    if (theReturnStatement->getNumReturns() == 1) {
        Assign *a = (Assign*)*theReturnStatement->getReturns().begin();
        signature->setRetType(a->getType());
    }

    Boomerang::get()->alert_decompile_debug_point(this, "after removing unused and redundant returns");
    return removedRets || removedParams;
}

/***************************************************************************//**
 *
 * \brief Update parameters and call livenesses to take into account the changes
 * causes by removing a return from this procedure, or a callee's parameter
 * (which affects this procedure's arguments, which are also uses).
 *
 * Need to save the old parameters and call livenesses, redo the dataflow and
 * removal of unused statements, recalculate the parameters and call livenesses,
 * and if either or both of these are changed, recurse to parents or those calls'
 * children respectively. (When call livenesses change like this, it means that
 * the recently removed return was the only use of that liveness, i.e. there was a
 * return chain.)
 * \sa removeRedundantReturns().
 *
 ******************************************************************************/
void UserProc::updateForUseChange(std::set<UserProc*>& removeRetSet) {
    // We need to remember the parameters, and all the livenesses for all the calls, to see if these are changed
    // by removing returns
    if (DEBUG_UNUSED) {
        LOG << "%%% updating " << getName() << " for changes to uses (returns or arguments)\n";
        LOG << "%%% updating dataflow:\n";
    }

    // Save the old parameters and call liveness
    StatementList oldParameters(parameters);
    std::map<CallStatement*, UseCollector> callLiveness;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    BB_IT it;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
        CallStatement* c = (CallStatement*) bb->getLastStmt(rrit, srit);
        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr || !c->isCall()) continue;
        UserProc* dest = (UserProc*)c->getDestProc();
        // Not interested in unanalysed indirect calls (not sure) or calls to lib procs
        if (dest == nullptr || dest->isLib()) continue;
        callLiveness[c].makeCloneOf(*c->getUseCollector());
    }

    // Have to redo dataflow to get the liveness at the calls correct
    removeCallLiveness();            // Want to recompute the call livenesses
    doRenameBlockVars(-3, true);

    remUnusedStmtEtc();                // Also redoes parameters

    // Have the parameters changed? If so, then all callers will need to update their arguments, and do similar
    // analysis to the removal of returns
    //findFinalParameters();
    removeRedundantParameters();
    if (parameters.size() != oldParameters.size()) {
        if (DEBUG_UNUSED)
            LOG << "%%%  parameters changed for " << getName() << "\n";
        std::set<CallStatement*>& callers = getCallers();
        std::set<CallStatement*>::iterator cc;
        std::set<UserProc*> callerProcs;
        std::set<UserProc*>::iterator pp;
        for (cc = callers.begin(); cc != callers.end(); ++cc) {
            (*cc)->updateArguments();
            // Schedule the callers for analysis
            removeRetSet.insert((*cc)->getProc());
        }
    }
    // Check if the liveness of any calls has changed
    std::map<CallStatement*, UseCollector>::iterator ll;
    for (ll = callLiveness.begin(); ll != callLiveness.end(); ++ll) {
        CallStatement* call = ll->first;
        UseCollector& oldLiveness = ll->second;
        UseCollector& newLiveness = *call->getUseCollector();
        if (!(newLiveness == oldLiveness)) {
            if (DEBUG_UNUSED)
                LOG << "%%%  liveness for call to " << call->getDestProc()->getName() << " in " << getName() <<
                       " changed\n";
            removeRetSet.insert((UserProc*)call->getDestProc());
        }
    }
}
//! Clear the useCollectors (in this Proc, and all calls).
void UserProc::clearUses() {
    if (VERBOSE)
        LOG << "### clearing usage for " << getName() << " ###\n";
    col.clear();
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    for (it = cfg->begin(); it != cfg->end(); ++it) {
        CallStatement* c = (CallStatement*) (*it)->getLastStmt(rrit, srit);
        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr || !c->isCall()) continue;
        c->clearUseCollector();
    }
}

/***************************************************************************//**
 *
 * \brief Global type analysis (for this procedure).
 *
 ******************************************************************************/
void UserProc::typeAnalysis() {
    if (VERBOSE)
        LOG << "### type analysis for " << getName() << " ###\n";

    // Now we need to add the implicit assignments. Doing this earlier is extremely problematic, because
    // of all the m[...] that change their sorting order as their arguments get subscripted or propagated into
    // Do this regardless of whether doing dfa-based TA, so things like finding parameters can rely on implicit assigns
    addImplicitAssigns();

    // Data flow based type analysis
    // Want to be after all propagation, but before converting expressions to locals etc
    if (DFA_TYPE_ANALYSIS) {
        if (VERBOSE || DEBUG_TA)
            LOG << "--- start data flow based type analysis for " << getName() << " ---\n";

        bool first = true;
        do {
            if (!first) {
                doRenameBlockVars(-1, true);        // Subscript the discovered extra parameters
                //propagateAtDepth(maxDepth);        // Hack: Can sometimes be needed, if call was indirect
                bool convert;
                propagateStatements(convert, 0);
            }
            first = false;
            dfaTypeAnalysis();

            // There used to be a pass here to insert casts. This is best left until global type analysis is complete,
            // so do it just before translating from SSA form (which is the where type information becomes inaccessible)

        } while (ellipsisProcessing());
        simplify();                        // In case there are new struct members
        if (VERBOSE || DEBUG_TA)
            LOG << "=== end type analysis for " << getName() << " ===\n";
    }

    else if (CON_TYPE_ANALYSIS) {
        // FIXME: if we want to do comparison
    }

    printXML();
}

void UserProc::clearRanges() {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++)
        (*it)->clearRanges();
}

/***************************************************************************//**
 *
 * \brief Range analysis (for this procedure).
 *
 ******************************************************************************/
void UserProc::rangeAnalysis() {
    std::cout << "performing range analysis on " << getName() << "\n";

    // this helps
    cfg->sortByAddress();

    cfg->addJunctionStatements();
    cfg->establishDFTOrder();

    clearRanges();

    if (VERBOSE) {
        LOG << "=== Before performing range analysis for " << getName() << " ===\n"
            << *this
            << "=== end before performing range analysis for " << getName() << " ===\n\n";
    }

    std::list<Statement*> execution_paths;
    std::list<Statement*> junctions;

    assert(cfg->getEntryBB());
    assert(cfg->getEntryBB()->getFirstStmt());
    execution_paths.push_back(cfg->getEntryBB()->getFirstStmt());

    int watchdog = 0;

    while(execution_paths.size()) {
        while(execution_paths.size()) {
            Statement *stmt = execution_paths.front();
            execution_paths.pop_front();
            if (stmt == nullptr)
                continue;  // ??
            if (stmt->isJunction())
                junctions.push_back(stmt);
            else
                stmt->rangeAnalysis(execution_paths);
        }
        if (watchdog > 45)
            LOG << "processing execution paths resulted in " << (int)junctions.size() << " junctions to process\n";
        while(junctions.size()) {
            Statement *junction = junctions.front();
            junctions.pop_front();
            if (watchdog > 45)
                LOG << "processing junction " << junction << "\n";
            assert(junction->isJunction());
            junction->rangeAnalysis(execution_paths);
        }

        watchdog++;
        if (watchdog > 10) {
            LOG << "  watchdog " << watchdog << "\n";
            if (watchdog > 45) {
                LOG << (int)execution_paths.size() << " execution paths remaining.\n"
                    << "=== After range analysis watchdog " << watchdog << " for " << getName() << " ===\n"
                    << *this
                    << "=== end after range analysis watchdog " << watchdog << " for " << getName() << " ===\n\n";
            }
        }
        if (watchdog > 50) {
            LOG << "  watchdog expired\n";
            break;
        }
    }

    LOG << "=== After range analysis for " << getName() << " ===\n"
        << *this
        << "=== end after range analysis for " << getName() << " ===\n\n";

    cfg->removeJunctionStatements();
}

/***************************************************************************//**
 *
 * \brief Detect and log possible buffer overflows
 *
 ******************************************************************************/
void UserProc::logSuspectMemoryDefs() {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++)
        if ((*it)->isAssign()) {
            Assign *a = (Assign*)*it;
            if (a->getLeft()->isMemOf()) {
                RangeMap &rm = a->getRanges();
                Exp *p = rm.substInto(a->getLeft()->getSubExp1()->clone());
                if (rm.hasRange(p)) {
                    Range &r = rm.getRange(p);
                    LOG << "got p " << p << " with range " << r << "\n";
                    if (r.getBase()->getOper() == opInitValueOf &&
                            r.getBase()->getSubExp1()->isRegOfK() &&
                            ((Const*)r.getBase()->getSubExp1()->getSubExp1())->getInt() == 28) {
                        RTL *rtl = a->getBB()->getRTLWithStatement(a);
                        LOG << "interesting stack reference at " << rtl->getAddress() << " " << a << "\n";
                    }
                }
            }
        }
}

RTL* globalRtl = 0;
/***************************************************************************//**
 *
 * \brief Copy the decoded indirect control transfer instructions' RTLs to
 * the front end's map, and decode any new targets for this CFG
 *
 * Copy the RTLs for the already decoded Indirect Control Transfer instructions, and decode any new targets in this CFG
 * Note that we have to delay the new target decoding till now, because otherwise we will attempt to decode nested
 * switch statements without having any SSA renaming, propagation, etc
 *
 *
 ******************************************************************************/

void UserProc::processDecodedICTs() {
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    for (BasicBlock * bb = cfg->getFirstBB(it); bb; bb = cfg->getNextBB(it)) {
        Statement* last = bb->getLastStmt(rrit, srit);
        if (last == nullptr) continue;            // e.g. a BB with just a NOP in it
        if (!last->isHL_ICT()) continue;
        RTL* rtl = bb->getLastRtl();
        if (DEBUG_SWITCH)
            LOG << "Saving high level switch statement " << rtl << "\n";
        prog->addDecodedRtl(bb->getHiAddr(), rtl);
        // Now decode those new targets, adding out edges as well
        //        if (last->isCase())
        //            bb->processSwitch(this);
    }
}

// Find or insert a new implicit reference just before statement s, for address expression a with type t.
// Meet types if necessary
/// Find and if necessary insert an implicit reference before s whose address expression is a and type is t.
void UserProc::setImplicitRef(Statement* s, Exp* a, Type* ty) {
    BasicBlock * bb = s->getBB();            // Get s' enclosing BB
    std::list<RTL*> *rtls = bb->getRTLs();
    for (std::list<RTL*>::iterator rit = rtls->begin(); rit != rtls->end(); rit++) {
        RTL::iterator it, itForS;
        RTL* rtlForS;
        for (it = (*rit)->begin(); it != (*rit)->end(); it++) {
            if (*it == s ||
                    // Not the searched for statement. But if it is a call or return statement, it will be the last, and
                    // s must be a substatement (e.g. argument, return, define, etc).
                    ((*it)->isCall() || (*it)->isReturn())) {
                // Found s. Search preceeding statements for an implicit reference with address a
                itForS = it;
                rtlForS = *rit;
                bool found = false;
                bool searchEarlierRtls = true;
                while (it != (*rit)->begin()) {
                    ImpRefStatement* irs = (ImpRefStatement*) *--it;
                    if (!irs->isImpRef()) {
                        searchEarlierRtls = false;
                        break;
                    }
                    if (*irs->getAddressExp() == *a) {
                        found = true;
                        searchEarlierRtls = false;
                        break;
                    }
                }
                while (searchEarlierRtls && rit != rtls->begin()) {
                    for (std::list<RTL*>::reverse_iterator revit = rtls->rbegin(); revit != rtls->rend(); ++revit) {
                        it = (*revit)->end();
                        while (it != (*revit)->begin()) {
                            ImpRefStatement* irs = (ImpRefStatement*) *--it;
                            if (!irs->isImpRef()) {
                                searchEarlierRtls = false;
                                break;
                            }
                            if (*irs->getAddressExp() == *a) {
                                found = true;
                                searchEarlierRtls = false;
                                break;
                            }
                        }
                        if (!searchEarlierRtls) break;
                    }
                }
                if (found) {
                    ImpRefStatement* irs = (ImpRefStatement*)*it;
                    bool ch;
                    irs->meetWith(ty, ch);
                } else {
                    ImpRefStatement* irs = new ImpRefStatement(ty, a);
                    rtlForS->insert(itForS,irs);
                }
                return;
            }
        }
    }
    assert(0);                // Could not find s withing its enclosing BB
}
//! eliminate duplicate arguments
void UserProc::eliminateDuplicateArgs() {
    if (VERBOSE)
        LOG << "### eliminate duplicate args for " << getName() << " ###\n";
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    for (it = cfg->begin(); it != cfg->end(); ++it) {
        CallStatement* c = (CallStatement*) (*it)->getLastStmt(rrit, srit);
        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr || !c->isCall()) continue;
        c->eliminateDuplicateArgs();
    }
}
//! Remove all liveness info in UseCollectors in calls
void UserProc::removeCallLiveness() {
    if (VERBOSE)
        LOG << "### removing call livenesses for " << getName() << " ###\n";
    BB_IT it;
    BasicBlock::rtlrit rrit; StatementList::reverse_iterator srit;
    for (it = cfg->begin(); it != cfg->end(); ++it) {
        CallStatement* c = (CallStatement*) (*it)->getLastStmt(rrit, srit);
        // Note: we may have removed some statements, so there may no longer be a last statement!
        if (c == nullptr || !c->isCall()) continue;
        c->removeAllLive();
    }
}

void UserProc::mapTempsToLocals() {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    TempToLocalMapper ttlm(this);
    StmtExpVisitor sv(&ttlm);
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        s->accept(&sv);
    }
}

// For debugging:
void dumpProcList(ProcList* pc) {
    ProcList::iterator pi;
    for (pi = pc->begin(); pi != pc->end(); ++pi)
        std::cerr << (*pi)->getName() << ", ";
    std::cerr << "\n";
}

void dumpProcSet(ProcSet* pc) {
    ProcSet::iterator pi;
    for (pi = pc->begin(); pi != pc->end(); ++pi)
        std::cerr << (*pi)->getName() << ", ";
    std::cerr << "\n";
}
/// Set an equation as proven. Useful for some sorts of testing
void Proc::setProvenTrue(Exp* fact) {
    assert(fact->isEquality());
    Exp* lhs = ((Binary*)fact)->getSubExp1();
    Exp* rhs = ((Binary*)fact)->getSubExp2();
    provenTrue[lhs] = rhs;
}

//! Map expressions to locals and initial parameters
void UserProc::mapLocalsAndParams() {
    Boomerang::get()->alert_decompile_debug_point(this, "before mapping locals from dfa type analysis");
    if (DEBUG_TA)
        LOG << " ### mapping expressions to local variables for " << getName() << " ###\n";
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        Statement* s = *it;
        s->dfaMapLocals();
    }
    if (DEBUG_TA)
        LOG << " ### end mapping expressions to local variables for " << getName() << " ###\n";
}

void UserProc::makeSymbolsImplicit() {
    SymbolMap::iterator it;
    SymbolMap sm2 = symbolMap;            // Copy the whole map; necessary because the keys (Exps) change
    symbolMap.clear();
    ImplicitConverter ic(cfg);
    for (it = sm2.begin(); it != sm2.end(); ++it) {
        Exp* impFrom = const_cast<Exp *>(it->first)->accept(&ic);
        mapSymbolTo(impFrom, it->second);
    }
}

void UserProc::makeParamsImplicit() {
    StatementList::iterator it;
    ImplicitConverter ic(cfg);
    for (it = parameters.begin(); it != parameters.end(); ++it) {
        Exp* lhs = ((Assignment*)*it)->getLeft();
        lhs = lhs->accept(&ic);
        ((Assignment*)*it)->setLeft(lhs);
    }
}

void UserProc::findLiveAtDomPhi(LocationSet& usedByDomPhi) {
    LocationSet usedByDomPhi0;
    std::map<Exp*, PhiAssign*, lessExpStar> defdByPhi;
    df.findLiveAtDomPhi(0, usedByDomPhi, usedByDomPhi0, defdByPhi);
    // Note that the above is not the complete algorithm; it has found the dead phi-functions in the defdAtPhi
    std::map<Exp*, PhiAssign*, lessExpStar>::iterator it;
    for (it = defdByPhi.begin(); it != defdByPhi.end(); ++it) {
        // For each phi parameter, remove from the final usedByDomPhi set
        PhiAssign::iterator pp;
        for (pp = it->second->begin(); pp != it->second->end(); ++pp) {
            if (pp->e == nullptr) continue;
            RefExp* wrappedParam = new RefExp(pp->e, pp->def);
            usedByDomPhi.remove(wrappedParam);
        }
        // Now remove the actual phi-function (a PhiAssign Statement)
        // Ick - some problem with return statements not using their returns until more analysis is done
        //removeStatement(it->second);
    }
}

#if USE_DOMINANCE_NUMS
void UserProc::setDominanceNumbers() {
    int currNum = 1;
    df.setDominanceNums(0, currNum);
}
#endif
//! Find the locations united by Phi-functions
void UserProc::findPhiUnites(ConnectionGraph& pu) {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        PhiAssign* pa = (PhiAssign*)*it;
        if (!pa->isPhi()) continue;
        Exp* lhs = pa->getLeft();
        RefExp* reLhs = new RefExp(lhs, pa);
        PhiAssign::iterator pp;
        for (pp = pa->begin(); pp != pa->end(); ++pp) {
            if (pp->e == nullptr) continue;
            RefExp* re = new RefExp(pp->e, pp->def);
            pu.connect(reLhs, re);
        }
    }
}
// WARN: write tests for getRegName in all combinations of r[1] r[tmp+1] etc.
/// Get a name like eax or o2 from r24 or r8
const char* UserProc::getRegName(Exp* r) {
    assert(r->isRegOf());

    //assert(r->getSubExp1()->isConst());
    if(r->getSubExp1()->isConst()) {
        int regNum = ((Const*)r->getSubExp1())->getInt();
        const char* regName = prog->getRegName(regNum);
        if (regName[0] == '%') regName++;        // Skip % if %eax
        return regName;
    }
    LOG_VERBOSE(2) << "warning - UserProc::getRegName(Exp* r) will try to build register name from [tmp+X] !";
    // TODO: how to handle register file lookups ?
    // in some cases the form might be r[tmp+value]
    // just return this expression :(
    //WARN: this is a hack to prevent crashing when r->subExp1 is not const
    std::ostringstream ostr;

    r->getSubExp1()->print(ostr);

    return strdup(ostr.str().c_str());
}
//! Find the type of the local or parameter \a e
Type* UserProc::getTypeForLocation(const Exp* e) {
    const char* name=((const Const*)((const Unary*)e)->getSubExp1())->getStr();
    if (e->isLocal()) {
        if (locals.find(name) != locals.end())
            return locals[name];
    }
    // Sometimes parameters use opLocal, so fall through
    return getParamType(name);
}
const Type* UserProc::getTypeForLocation(const Exp* e) const {
    return const_cast<UserProc *>(this)->getTypeForLocation(e);
}
/***************************************************************************//**
 *
 * \brief Add a mapping for the destinations of phi functions that have one
 * argument that is a parameter
 *
 * The idea here is to give a name to those SSA variables that have one and only one parameter amongst the phi
 * arguments. For example, in test/source/param1, there is 18 *v* m[r28{-} + 8] := phi{- 7} with m[r28{-} + 8]{0} mapped
 * to param1; insert a mapping for m[r28{-} + 8]{18} to param1. This will avoid a copy, and will use the name of the
 * parameter only when it is acually used as a parameter
 *
 * \returns the cycle set from the recursive call to decompile()
 *
 ******************************************************************************/
void UserProc::nameParameterPhis() {
    StatementList stmts;
    getStatements(stmts);
    StatementList::iterator it;
    for (it = stmts.begin(); it != stmts.end(); it++) {
        PhiAssign* pi = (PhiAssign*)*it;
        if (!pi->isPhi())
            continue;            // Might be able to optimise this a bit
        // See if the destination has a symbol already
        Exp* lhs = pi->getLeft();
        RefExp* lhsRef = new RefExp(lhs, pi);
        if (findFirstSymbol(lhsRef) != nullptr)
            continue;                        // Already mapped to something
        bool multiple = false;                // True if find more than one unique parameter
        const char* firstName = nullptr;                // The name for the first parameter found
        Type* ty = pi->getType();
        PhiAssign::iterator pp;
        for (pp = pi->begin(); pp != pi->end(); ++pp) {
            if (pp->def->isImplicit()) {
                RefExp* phiArg = new RefExp(pp->e, pp->def);
                const char* name = lookupSym(phiArg, ty);
                if (name != nullptr) {
                    if (firstName != nullptr && strcmp(firstName, name) != 0) {
                        multiple = true;
                        break;
                    }
                    firstName = name;        // Remember this candidate
                }
            }
        }
        if (multiple || firstName == nullptr)
            continue;
        mapSymbolTo(lhsRef, Location::param(firstName, this));
    }
}
//! True if a local exists with name \a name
bool UserProc::existsLocal(const char* name) {
    std::string s(name);
    return locals.find(s) != locals.end();
}
//! Check if \a r is already mapped to a local, else add one
void UserProc::checkLocalFor(RefExp* r) {
    if (lookupSymFromRefAny(r))
        return; // Already have a symbol for r
    Statement* def = r->getDef();
    if (!def)
        return; //TODO: should this be logged ?
    Exp* base = r->getSubExp1();
    Type* ty = def->getTypeFor(base);
    // No, get its name from the front end
    const char* locName = nullptr;
    if (base->isRegOf()) {
        locName = getRegName(base);
        // Create a new local, for the base name if it doesn't exist yet, so we don't need several names for the
        // same combination of location and type. However if it does already exist, addLocal will allocate a
        // new name. Example: r8{0}->argc type int, r8->o0 type int, now r8->o0_1 type char*.
        if (existsLocal(locName))
            locName = newLocalName(r);
    }
    else {
        locName = newLocalName(r);
    }
    addLocal(ty, locName, base);
}


//    -    -    -    -    -    -    -    -    -

#ifdef USING_MEMOS
class LibProcMemo : public Memo {
public:
    LibProcMemo(int mId) : Memo(mId) { }

    bool visited;
    Prog *prog;
    Signature *signature;                        // r
    ADDRESS address;
    Proc *m_firstCaller;
    ADDRESS m_firstCallerAddr;
    std::set<Exp*, lessExpStar> provenTrue;            // r
    std::set<CallStatement*> callerSet;
    Cluster *cluster;
};

Memo *LibProc::makeMemo(int mId) {
    LibProcMemo *m = new LibProcMemo(mId);
    m->visited = visited;
    m->prog = prog;
    m->signature = signature;
    m->address = address;
    m->m_firstCaller = m_firstCaller;
    m->m_firstCallerAddr = m_firstCallerAddr;
    m->provenTrue = provenTrue;
    m->callerSet = callerSet;
    m->cluster = cluster;

    //    signature->takeMemo(mId);
    //    for (std::set<Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); it++)
    //        (*it)->takeMemo(mId);

    return m;
}

void LibProc::readMemo(Memo *mm, bool dec) {
    LibProcMemo *m = dynamic_cast<LibProcMemo*>(mm);
    visited = m->visited;
    prog = m->prog;
    signature = m->signature;
    address = m->address;
    m_firstCaller = m->m_firstCaller;
    m_firstCallerAddr = m->m_firstCallerAddr;
    provenTrue = m->provenTrue;
    callerSet = m->callerSet;
    cluster = m->cluster;

    //    signature->restoreMemo(m->mId, dec);
    //    for (std::set<Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); it++)
    //        (*it)->restoreMemo(m->mId, dec);
}

class UserProcMemo : public Memo {
public:
    UserProcMemo(int mId) : Memo(mId) { }

    bool visited;
    Prog *prog;
    Signature *signature;                        // r
    ADDRESS address;
    Proc *m_firstCaller;
    ADDRESS m_firstCallerAddr;
    std::set<Exp*, lessExpStar> provenTrue;            // r
    std::set<CallStatement*> callerSet;
    Cluster *cluster;

    Cfg* cfg;
    ProcStatus status;
    std::map<std::string, Type*> locals;        // r
    UserProc::SymbolMap symbolMap;            // r
    std::list<Proc*> calleeList;
};

Memo *UserProc::makeMemo(int mId) {
    UserProcMemo *m = new UserProcMemo(mId);
    m->visited = visited;
    m->prog = prog;
    m->signature = signature;
    m->address = address;
    m->m_firstCaller = m_firstCaller;
    m->m_firstCallerAddr = m_firstCallerAddr;
    m->provenTrue = provenTrue;
    m->callerSet = callerSet;
    m->cluster = cluster;

    m->cfg = cfg;
    m->status = status;
    m->locals = locals;
    m->symbolMap = symbolMap;
    m->calleeList = calleeList;

    signature->takeMemo(mId);
    for (std::set<Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); it++)
        (*it)->takeMemo(mId);

    for (std::map<std::string, Type*>::iterator it = locals.begin(); it != locals.end(); it++)
        (*it).second->takeMemo(mId);

    for (SymbolMap::iterator it = symbolMap.begin(); it != symbolMap.end(); it++) {
        (*it).first->takeMemo(mId);
        (*it).second->takeMemo(mId);
    }

    return m;
}

void UserProc::readMemo(Memo *mm, bool dec) {
    UserProcMemo *m = dynamic_cast<UserProcMemo*>(mm);
    visited = m->visited;
    prog = m->prog;
    signature = m->signature;
    address = m->address;
    m_firstCaller = m->m_firstCaller;
    m_firstCallerAddr = m->m_firstCallerAddr;
    provenTrue = m->provenTrue;
    callerSet = m->callerSet;
    cluster = m->cluster;

    cfg = m->cfg;
    status = m->status;
    locals = m->locals;
    symbolMap = m->symbolMap;
    calleeList = m->calleeList;

    signature->restoreMemo(m->mId, dec);
    for (std::set<Exp*, lessExpStar>::iterator it = provenTrue.begin(); it != provenTrue.end(); it++)
        (*it)->restoreMemo(m->mId, dec);

    for (std::map<std::string, Type*>::iterator it = locals.begin(); it != locals.end(); it++)
        (*it).second->restoreMemo(m->mId, dec);

    for (SymbolMap::iterator it = symbolMap.begin(); it != symbolMap.end(); it++) {
        (*it).first->restoreMemo(m->mId, dec);
        (*it).second->restoreMemo(m->mId, dec);
    }
}
#endif        // #ifdef USING_MEMOS
// 3) Check implicit assigns for parameter and global types.
void UserProc::dfa_analyze_implict_assigns( Statement* s, Prog* prog )
{
    bool allZero;
    Exp *lhs;
    Exp *slhs;
    Type *iType;
    int i;

    if (!s->isImplicit())
        return;

    lhs = ((ImplicitAssign*)s)->getLeft();
    // Note: parameters are not explicit any more
    //if (lhs->isParam()) {	// }
    slhs = lhs->clone()->removeSubscripts(allZero);
    iType = ((ImplicitAssign*)s)->getType();
    i = signature->findParam(slhs);
    if (i != -1)
        setParamType(i, iType);
    else if (lhs->isMemOf()) {
        Exp* sub = ((Location*)lhs)->getSubExp1();
        if (sub->isIntConst()) {
            // We have a m[K] := -
            ADDRESS K = ((Const*)sub)->getAddr();
            prog->globalUsed(K, iType);
        }
    }
    else if (lhs->isGlobal()) {
        assert(dynamic_cast<Location*>(lhs)!=nullptr);
        const char* gname = ((Const*)lhs->getSubExp1())->getStr();
        prog->setGlobalType(gname, iType);
    }
}
// m[idx*K1 + K2]; leave idx wild
static Exp* scaledArrayPat = Location::memOf(
    new Binary(opPlus,
        new Binary(opMult,
            new Terminal(opWild),
            new Terminal(opWildIntConst)),
        new Terminal(opWildIntConst)));

void UserProc::dfa_analyze_scaled_array_ref( Statement* s, Prog* prog )
{
    Exp *arr;
    std::list<Exp*> result;
    const char* nam;
    s->searchAll(scaledArrayPat, result);
    // query: (memOf (opPlus (opMult ? ?:IntConst) ?:IntConst))
    // rewrite_as (opArrayIndex (global `(getOrCreateGlobalName arg3) ) arg2 ) assert (= (typeSize (getOrCreateGlobalName arg3)) (arg1))
    for (Exp * rr : result)
    {
        //Type* ty = s->getTypeFor(*rr);
        // FIXME: should check that we use with array type...
        // Find idx and K2
        assert(((Unary*)rr)->getSubExp1() == rr->getSubExp1());
        Exp* t = rr->getSubExp1();		// idx*K1 + K2
        Exp* l = ((Binary*)t)->getSubExp1();		// idx*K1
        Exp* r = ((Binary*)t)->getSubExp2();		// K2
        ADDRESS K2 = ((Const*)r)->getAddr();
        Exp* idx = ((Binary*)l)->getSubExp1();

        // Replace with the array expression
        nam = prog->getGlobalName(K2);
        if (nam == nullptr)
            nam = prog->newGlobalName(K2);
        arr = new Binary(opArrayIndex,
            Location::global(nam, this),
            idx
            );
        if (s->searchAndReplace(scaledArrayPat, arr)) {
            if (s->isImplicit())
                // Register an array of appropriate type
                prog->globalUsed(K2, new ArrayType(((ImplicitAssign*)s)->getType()));
        }
    }
}
Log& operator<< ( Log& out, const UserProc& c ) {
    std::ostringstream ost;
    c.print(ost);
    out << ost.str().c_str();
    return out;
}
