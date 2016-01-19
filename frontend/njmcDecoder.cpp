/*
 * Copyright (C) 1998-2001, The University of Queensland
 * Copyright (C) 2001, Sun Microsystems, Inc
 * Copyright (C) 2002, Trent Waddington
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/***************************************************************************/ /**
  * \file       njmcDecoder.cpp
  * \brief   This file contains the machine independent decoding functionality.
  ******************************************************************************/
#include "decoder.h"
#include "rtl.h"
#include "exp.h"
#include "register.h"
#include "cfg.h"
#include "proc.h"
#include "prog.h"
#include "BinaryFile.h"
#include "boomerang.h"
#include "util.h"

#include <cassert>
#include <cstdarg> // For varargs
#include <cstring>
/**********************************
 * NJMCDecoder methods.
 **********************************/

/***************************************************************************/ /**
  * \fn       NJMCDecoder::NJMCDecoder
  * \brief
  * \param       prog: Pointer to the Prog object
  *
  ******************************************************************************/
NJMCDecoder::NJMCDecoder(Prog *prg) : prog(prg),Image(Boomerang::get()->getImage()) {}

/***************************************************************************/ /**
  * \brief   Given an instruction name and a variable list of expressions representing the actual operands of
  *              the instruction, use the RTL template dictionary to return the instantiated RTL representing the
  *              semantics of the instruction. This method also displays a disassembly of the instruction if the
  *              relevant compilation flag has been set.
  * \param   pc  native PC
  * \param   name - instruction name
  * \param   ... - Semantic String ptrs representing actual operands
  * \returns an instantiated list of Exps
  ******************************************************************************/
std::list<Instruction *> *NJMCDecoder::instantiate(ADDRESS pc, const char * format, ...) {
    // Get the signature of the instruction and extract its parts
    std::pair<QString, unsigned> sig = RTLDict.getSignature(format);
    QString opcode = sig.first;
    unsigned numOperands = sig.second;

    // Put the operands into a vector
    std::vector<Exp *> actuals(numOperands);
    va_list args;
    va_start(args, format);
    for (unsigned i = 0; i < numOperands; i++)
        actuals[i] = va_arg(args, Exp *);
    va_end(args);

    if (DEBUG_DECODER) {
        QTextStream q_cout(stdout);
        // Display a disassembly of this instruction if requested
        q_cout << pc << ": " << format << " ";
        for (std::vector<Exp *>::iterator itd = actuals.begin(); itd != actuals.end(); itd++) {
            if ((*itd)->isIntConst()) {
                int val = ((Const *)(*itd))->getInt();
                if (val > 100 || val < -100)
                    q_cout << "0x" << QString::number(val,16);
                else
                    q_cout << val;
            } else
                (*itd)->print(q_cout);
            if (itd != actuals.end() - 1)
                q_cout << ", ";
        }
        q_cout << '\n';
    }

    std::list<Instruction *> *instance = RTLDict.instantiateRTL(opcode, pc, actuals);

    return instance;
}

/***************************************************************************/ /**
  * \brief   Similarly to NJMCDecoder::instantiate, given a parameter name and a list of Exp*'s representing
  * sub-parameters, return a fully substituted Exp for the whole expression
  * \note    Caller must delete result
  * \param   name - parameter name
  *          ... - Exp* representing actual operands
  * \returns an instantiated list of Exps
  ******************************************************************************/
Exp *NJMCDecoder::instantiateNamedParam(char *name, ...) {
    if (RTLDict.ParamSet.find(name) == RTLDict.ParamSet.end()) {
        LOG_STREAM() << "No entry for named parameter '" << name << "'\n";
        return nullptr;
    }
    assert(RTLDict.DetParamMap.find(name) != RTLDict.DetParamMap.end());
    ParamEntry &ent = RTLDict.DetParamMap[name];
    if (ent.kind != PARAM_ASGN && ent.kind != PARAM_LAMBDA) {
        LOG_STREAM() << "Attempt to instantiate expressionless parameter '" << name << "'\n";
        return nullptr;
    }
    // Start with the RHS
    assert(ent.asgn->getKind() == STMT_ASSIGN);
    Exp *result = ((Assign *)ent.asgn)->getRight()->clone();

    va_list args;
    va_start(args, name);
    for (auto &elem : ent.params) {
        Location formal(opParam, Const::get(elem), nullptr);
        Exp *actual = va_arg(args, Exp *);
        bool change;
        result = result->searchReplaceAll(formal, actual, change);
    }
    return result;
}

/***************************************************************************/ /**
  * \brief   In the event that it's necessary to synthesize the call of a named parameter generated with
  *          instantiateNamedParam(), this substituteCallArgs() will substitute the arguments that follow into
  *          the expression.
  * \note    Should only be used after instantiateNamedParam(name, ..);
  * \note    exp (the pointer) could be changed
  * \param   name - parameter name
  * \param   exp - expression to instantiate into
  * \param   ... - Exp* representing actual operands
  * \returns an instantiated list of Exps
  ******************************************************************************/
Exp * NJMCDecoder::substituteCallArgs(char *name, Exp * exp, ...) {
    if (RTLDict.ParamSet.find(name) == RTLDict.ParamSet.end()) {
        LOG_STREAM() << "No entry for named parameter '" << name << "'\n";
        return exp;
    }
    ParamEntry &ent = RTLDict.DetParamMap[name];
    /*if (ent.kind != PARAM_ASGN && ent.kind != PARAM_LAMBDA) {
                LOG_STREAM() << "Attempt to instantiate expressionless parameter '" << name << "'\n";
                return;
        }*/

    va_list args;
    va_start(args, exp);
    for (auto &elem : ent.funcParams) {
        Location formal(opParam, Const::get(elem), nullptr);
        Exp *actual = va_arg(args, Exp *);
        bool change;
        exp = exp->searchReplaceAll(formal, actual, change);
    }
	return exp;
}

/***************************************************************************/ /**
  * \brief       Resets the fields of a DecodeResult to their default values.
  ******************************************************************************/
void DecodeResult::reset() {
    numBytes = 0;
    type = NCT;
    valid = true;
    rtl = nullptr;
    reDecode = false;
    forceOutEdge = ADDRESS::g(0L);
}

/***************************************************************************/ /**
  * These are functions used to decode instruction operands into
  * Exp*s.
  ******************************************************************************/

/***************************************************************************/ /**
  * \brief   Converts a numbered register to a suitable expression.
  * \param   regNum - the register number, e.g. 0 for eax
  * \returns the Exp* for the register NUMBER (e.g. "int 36" for %f4)
  ******************************************************************************/
Exp *NJMCDecoder::dis_Reg(int regNum) {
    Exp *expr = Location::regOf(regNum);
    return expr;
}

/***************************************************************************/ /**
  * \brief        Converts a number to a Exp* expression.
  * \param        num - a number
  * \returns             the Exp* representation of the given number
  ******************************************************************************/
Exp *NJMCDecoder::dis_Num(unsigned num) {
    Exp *expr = new Const(num); // TODO: what about signed values ?
    return expr;
}

/***************************************************************************/ /**
  * \brief   Process an unconditional jump instruction
  *              Also check if the destination is a label (MVE: is this done?)
  * \param   name name of instruction (for debugging)
  * \param   size size of instruction in bytes
  * \param   relocd
  * \param   delta
  * \param   pc native pc
  * \param   stmts list of statements (?)
  * \param   result ref to decoder result object
  ******************************************************************************/
void NJMCDecoder::unconditionalJump(const char *name, int size, ADDRESS relocd, ptrdiff_t delta, ADDRESS pc,
                                    std::list<Instruction *> *stmts, DecodeResult &result) {
    result.rtl = new RTL(pc, stmts);
    result.numBytes = size;
    GotoStatement *jump = new GotoStatement();
    jump->setDest((relocd - delta).native());
    result.rtl->appendStmt(jump);
    SHOW_ASM(name << " 0x" << relocd - delta)
}

/***************************************************************************/ /**
  * \brief   Process an indirect jump instruction
  * \param   name name of instruction (for debugging)
  * \param   size size of instruction in bytes
  * \param   dest destination Exp*
  * \param   pc native pc
  * \param   stmts list of statements (?)
  * \param   result ref to decoder result object
  ******************************************************************************/
void NJMCDecoder::computedJump(const char *name, int size, Exp *dest, ADDRESS pc, std::list<Instruction *> *stmts,
                               DecodeResult &result) {
    result.rtl = new RTL(pc, stmts);
    result.numBytes = size;
    GotoStatement *jump = new GotoStatement();
    jump->setDest(dest);
    jump->setIsComputed(true);
    result.rtl->appendStmt(jump);
    SHOW_ASM(name << " " << dest)
}

/***************************************************************************/ /**
  * \brief   Process an indirect call instruction
  * \param   name name of instruction (for debugging)
  * \param   size size of instruction in bytes
  * \param   dest destination Exp*
  * \param   pc native pc
  * \param   stmts list of statements (?)
  * \param   result ref to decoder result object
  ******************************************************************************/
void NJMCDecoder::computedCall(const char *name, int size, Exp *dest, ADDRESS pc, std::list<Instruction *> *stmts,
                               DecodeResult &result) {
    result.rtl = new RTL(pc, stmts);
    result.numBytes = size;
    CallStatement *call = new CallStatement();
    call->setDest(dest);
    call->setIsComputed(true);
    result.rtl->appendStmt(call);
    SHOW_ASM(name << " " << dest)
}
