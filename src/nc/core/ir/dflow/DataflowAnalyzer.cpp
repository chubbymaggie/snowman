/* The file is part of Snowman decompiler.             */
/* See doc/licenses.txt for the licensing information. */

//
// SmartDec decompiler - SmartDec is a native code to C/C++ decompiler
// Copyright (C) 2015 Alexander Chernov, Katerina Troshina, Yegor Derevenets,
// Alexander Fokin, Sergey Levin, Leonid Tsvetkov
//
// This file is part of SmartDec decompiler.
//
// SmartDec decompiler is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SmartDec decompiler is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SmartDec decompiler.  If not, see <http://www.gnu.org/licenses/>.
//

#include "DataflowAnalyzer.h"

#include <boost/unordered_map.hpp>

#include <nc/common/CancellationToken.h>
#include <nc/common/Foreach.h>
#include <nc/common/Warnings.h>

#include <nc/core/arch/Architecture.h>
#include <nc/core/arch/Instruction.h>
#include <nc/core/arch/Register.h>
#include <nc/core/ir/BasicBlock.h>
#include <nc/core/ir/CFG.h>
#include <nc/core/ir/Function.h>
#include <nc/core/ir/Jump.h>
#include <nc/core/ir/Statements.h>
#include <nc/core/ir/Terms.h>

#include "Dataflow.h"
#include "ExecutionContext.h"
#include "Value.h"

namespace nc {
namespace core {
namespace ir {
namespace dflow {

namespace {

template<class Map, class Pred>
void remove_if(Map &map, Pred pred) {
    auto i = map.begin();
    auto iend = map.end();

    while (i != iend) {
        if (pred(i->first)) {
            i = map.erase(i);
        } else {
            ++i;
        }
    }
}

} // anonymous namespace

void DataflowAnalyzer::analyze(const Function *function, const CancellationToken &canceled) {
    assert(function != NULL);

    /*
     * Returns true if the given term does not cover given memory location.
     */
    auto notCovered = [this](const MemoryLocation &mloc, const Term *term) -> bool {
        return !dataflow().getMemoryLocation(term).covers(mloc);
    };

    /* Control flow graph to run abstract interpretation loop on. */
    CFG cfg(function->basicBlocks());

    /* Mapping of a basic block to the definitions reaching its end. */
    boost::unordered_map<const BasicBlock *, ReachingDefinitions> outDefinitions;

    /*
     * Running abstract interpretation until reaching a fixpoint several times in a row.
     */
    int niterations = 0;
    int nfixpoints = 0;

    while (nfixpoints++ < 3) {
        /*
         * Run abstract interpretation on all basic blocks.
         */
        foreach (auto basicBlock, function->basicBlocks()) {
            ExecutionContext context(*this);

            /* Merge reaching definitions from predecessors. */
            foreach (const BasicBlock *predecessor, cfg.getPredecessors(basicBlock)) {
                context.definitions().merge(outDefinitions[predecessor]);
            }

            /* Remove definitions that do not cover the memory location that they define. */
            context.definitions().filterOut(notCovered);

            /* Execute all the statements in the basic block. */
            foreach (auto statement, basicBlock->statements()) {
                execute(statement, context);
            }

            /* Something has changed? */
            ReachingDefinitions &definitions(outDefinitions[basicBlock]);
            if (definitions != context.definitions()) {
                definitions = std::move(context.definitions());
                nfixpoints = 0;
            }
        }

        /*
         * Some terms might have changed their addresses. Filter again.
         */
        foreach (auto &termAndDefinitions, dataflow().term2definitions()) {
            termAndDefinitions.second.filterOut(notCovered);
        }

        /*
         * Do we loop infinitely?
         */
        if (++niterations >= 30) {
            ncWarning("Fixpoint was not reached after %1 iterations while analyzing dataflow. Giving up.", niterations);
            break;
        }

        canceled.poll();
    }

    /*
     * Remove information about terms that disappeared.
     * Terms can disappear if e.g. a call is deinstrumented during the analysis.
     */
    auto disappeared = [](const Term *term){ return term->statement()->basicBlock() == NULL; };

    std::vector<const Term *> disappearedTerms;
    foreach (auto &termAndDefinitions, dataflow().term2definitions()) {
        termAndDefinitions.second.filterOut([disappeared](const MemoryLocation &, const Term *term) { return disappeared(term); } );
    }

    remove_if(dataflow().term2value(), disappeared);
    remove_if(dataflow().term2location(), disappeared);
    remove_if(dataflow().term2definitions(), disappeared);
}

void DataflowAnalyzer::execute(const Statement *statement, ExecutionContext &context) {
    switch (statement->kind()) {
        case Statement::INLINE_ASSEMBLY:
            /*
             * To be completely correct, one should clear reaching definitions.
             * However, not doing this usually leads to better code.
             */
            break;
        case Statement::ASSIGNMENT: {
            auto assignment = statement->asAssignment();
            execute(assignment->right(), context);
            execute(assignment->left(), context);
            break;
        }
        case Statement::JUMP: {
            auto jump = statement->asJump();

            if (jump->condition()) {
                execute(jump->condition(), context);
            }
            if (jump->thenTarget().address()) {
                execute(jump->thenTarget().address(), context);
            }
            if (jump->elseTarget().address()) {
                execute(jump->elseTarget().address(), context);
            }
            break;
        }
        case Statement::CALL: {
            auto call = statement->asCall();
            execute(call->target(), context);
            break;
        }
        case Statement::RETURN: {
            break;
        }
        case Statement::TOUCH: {
            auto touch = statement->asTouch();
            execute(touch->term(), context);
            break;
        }
        case Statement::CALLBACK: {
            statement->asCallback()->function()();
            break;
        }
        default:
            ncWarning("Unknown statement kind: '%1'.", static_cast<int>(statement->kind()));
            break;
    }
}

void DataflowAnalyzer::execute(const Term *term, ExecutionContext &context) {
    assert(term != NULL);

    switch (term->kind()) {
        case Term::INT_CONST: {
            auto constant = term->asConstant();
            Value *value = dataflow().getValue(constant);
            value->setAbstractValue(constant->value());
            value->makeNotStackOffset();
            value->makeNotProduct();
            break;
        }
        case Term::INTRINSIC: {
            auto intrinsic = term->asIntrinsic();
            Value *value = dataflow().getValue(intrinsic);

            switch (intrinsic->intrinsicKind()) {
                case Intrinsic::UNKNOWN: /* FALLTHROUGH */
                case Intrinsic::UNDEFINED: {
                    value->setAbstractValue(AbstractValue(term->size(), -1, -1));
                    value->makeNotStackOffset();
                    value->makeNotProduct();
                    break;
                }
                case Intrinsic::ZERO_STACK_OFFSET: {
                    value->setAbstractValue(AbstractValue(term->size(), -1, -1));
                    value->makeStackOffset(0);
                    value->makeNotProduct();
                    break;
                }
                case Intrinsic::REACHING_SNAPSHOT: {
                    dataflow_.getDefinitions(intrinsic) = context.definitions();
                    break;
                }
                case Intrinsic::INSTRUCTION_ADDRESS: {
                    auto instruction = intrinsic->statement()->instruction();
                    value->setAbstractValue(SizedValue(term->size(), instruction->addr()));
                    value->makeNotStackOffset();
                    value->makeNotProduct();
                    break;
                }
                case Intrinsic::NEXT_INSTRUCTION_ADDRESS: {
                    auto instruction = intrinsic->statement()->instruction();
                    value->setAbstractValue(SizedValue(term->size(), instruction->addr() + instruction->size()));
                    value->makeNotStackOffset();
                    value->makeNotProduct();
                    break;
                }
                default: {
                    ncWarning("Unknown kind of intrinsic: '%1'", intrinsic->intrinsicKind());
                    break;
                }
            }
            break;
        }
        case Term::MEMORY_LOCATION_ACCESS: {
            auto access = term->asMemoryLocationAccess();
            setMemoryLocation(access, access->memoryLocation(), context);
            break;
        }
        case Term::DEREFERENCE: {
            auto dereference = term->asDereference();
            execute(dereference->address(), context);

            /* Compute memory location. */
            auto addressValue = dataflow().getValue(dereference->address());
            if (addressValue->abstractValue().isConcrete()) {
                if (dereference->domain() == MemoryDomain::MEMORY) {
                    setMemoryLocation(
                        dereference,
                        MemoryLocation(
                            dereference->domain(),
                            addressValue->abstractValue().asConcrete().value() * CHAR_BIT,
                            dereference->size()),
                        context);
                } else {
                    setMemoryLocation(
                        dereference,
                            MemoryLocation(
                                dereference->domain(),
                                addressValue->abstractValue().asConcrete().value(),
                                dereference->size()),
                        context);
                }
            } else if (addressValue->isStackOffset()) {
                setMemoryLocation(
                    dereference,
                    MemoryLocation(MemoryDomain::STACK, addressValue->stackOffset() * CHAR_BIT, dereference->size()),
                    context);
            } else {
                setMemoryLocation(dereference, MemoryLocation(), context);
            }
            break;
        }
        case Term::UNARY_OPERATOR:
            executeUnaryOperator(term->asUnaryOperator(), context);
            break;
        case Term::BINARY_OPERATOR:
            executeBinaryOperator(term->asBinaryOperator(), context);
            break;
        case Term::CHOICE: {
            auto choice = term->asChoice();
            execute(choice->preferredTerm(), context);
            execute(choice->defaultTerm(), context);

            if (!dataflow().getDefinitions(choice->preferredTerm()).empty()) {
                *dataflow().getValue(choice) = *dataflow().getValue(choice->preferredTerm());
            } else {
                *dataflow().getValue(choice) = *dataflow().getValue(choice->defaultTerm());
            }
            break;
        }
        default:
            ncWarning("Unknown term kind: '%1'.", static_cast<int>(term->kind()));
            break;
    }
}

void DataflowAnalyzer::setMemoryLocation(const Term *term, const MemoryLocation &newMemoryLocation, ExecutionContext &context) {
    auto oldMemoryLocation = dataflow().getMemoryLocation(term);

    /*
     * If the term has changed its location, remember the new location.
     */
    if (oldMemoryLocation != newMemoryLocation) {
        dataflow().setMemoryLocation(term, newMemoryLocation);

        /*
         * If the term is a write and had a memory location before,
         * reaching definitions can indicate that it defines the old
         * memory location. Fix this.
         */
        if (oldMemoryLocation && term->isWrite()) {
            context.definitions().filterOut(
                [term](const MemoryLocation &, const Term *definition) -> bool {
                    return definition == term;
                }
            );
        }
    }

    /*
     * If the term has a memory location and is not a global variable,
     * remember or update reaching definitions accordingly.
     */
    if (newMemoryLocation && !architecture()->isGlobalMemory(newMemoryLocation)) {
        if (term->isRead()) {
            auto &definitions = dataflow().getDefinitions(term);
            context.definitions().project(newMemoryLocation, definitions);
            mergeReachingValues(term, newMemoryLocation, definitions);
        }
        if (term->isWrite()) {
            context.definitions().addDefinition(newMemoryLocation, term);
        }
        if (term->isKill()) {
            context.definitions().killDefinitions(newMemoryLocation);
        }
    } else {
        if (term->isRead() && oldMemoryLocation) {
            dataflow().getDefinitions(term).clear();
        }
    }
}

void DataflowAnalyzer::mergeReachingValues(const Term *term, const MemoryLocation &termLocation, const ReachingDefinitions &definitions) {
    assert(term);
    assert(term->isRead());
    assert(termLocation);

    if (definitions.empty()) {
        return;
    }

    /*
     * Merge abstract values.
     */
    auto termValue = dataflow().getValue(term);
    auto termAbstractValue = termValue->abstractValue();

    foreach (const auto &chunk, definitions.chunks()) {
        assert(termLocation.covers(chunk.location()));

        /*
         * Mask of bits inside termAbstractValue which are covered by chunk's location.
         */
        auto mask = bitMask<ConstantValue>(chunk.location().size());
        if (architecture()->byteOrder() == ByteOrder::LittleEndian) {
            mask = bitShift(mask, chunk.location().addr() - termLocation.addr());
        } else {
            mask = bitShift(mask, termLocation.endAddr() - chunk.location().endAddr());
        }

        foreach (auto definition, chunk.definitions()) {
            auto definitionLocation = dataflow().getMemoryLocation(definition);
            assert(definitionLocation.covers(chunk.location()));

            auto definitionValue = dataflow().getValue(definition);
            auto definitionAbstractValue = definitionValue->abstractValue();

            /*
             * Shift definition's abstract value to match term's location.
             */
            if (architecture()->byteOrder() == ByteOrder::LittleEndian) {
                definitionAbstractValue.shift(definitionLocation.addr() - termLocation.addr());
            } else {
                definitionAbstractValue.shift(termLocation.endAddr() - definitionLocation.endAddr());
            }

            /* Project the value to the defined location. */
            definitionAbstractValue.project(mask);

            /* Update term's value. */
            termAbstractValue.merge(definitionAbstractValue);
        }
    }

    termValue->setAbstractValue(termAbstractValue.resize(term->size()));

    /*
     * Merge stack offset and product flags.
     *
     * Heuristic: merge information only from terms that define lower bits of the term's value.
     */
    const std::vector<const Term *> *lowerBitsDefinitions = NULL;

    if (architecture()->byteOrder() == ByteOrder::LittleEndian) {
        if (definitions.chunks().front().location().addr() == termLocation.addr()) {
            lowerBitsDefinitions = &definitions.chunks().front().definitions();
        }
    } else {
        if (definitions.chunks().back().location().endAddr() == termLocation.endAddr()) {
            lowerBitsDefinitions = &definitions.chunks().back().definitions();
        }
    }

    if (lowerBitsDefinitions) {
        foreach (auto definition, *lowerBitsDefinitions) {
            auto definitionValue = dataflow().getValue(definition);

            if (definitionValue->isNotStackOffset()) {
                termValue->makeNotStackOffset();
            } else if (definitionValue->isStackOffset()) {
                termValue->makeStackOffset(definitionValue->stackOffset());
            }

            if (definitionValue->isNotProduct()) {
                termValue->makeNotProduct();
            } else if (definitionValue->isProduct()) {
                termValue->makeProduct();
            }
        }
    }
}

void DataflowAnalyzer::executeUnaryOperator(const UnaryOperator *unary, ExecutionContext &context) {
    execute(unary->operand(), context);

    Value *value = dataflow().getValue(unary);
    Value *operandValue = dataflow().getValue(unary->operand());

    value->setAbstractValue(apply(unary, operandValue->abstractValue()).merge(value->abstractValue()));

    switch (unary->operatorKind()) {
        case UnaryOperator::SIGN_EXTEND:
        case UnaryOperator::ZERO_EXTEND:
        case UnaryOperator::TRUNCATE:
            if (operandValue->isNotStackOffset()) {
                value->makeNotStackOffset();
            } else if (operandValue->isStackOffset()) {
                value->makeStackOffset(operandValue->stackOffset());
            }
            if (operandValue->isNotProduct()) {
                value->makeNotProduct();
            } else if (operandValue->isProduct()) {
                value->makeProduct();
            }
            break;
        default:
            value->makeNotStackOffset();
            value->makeNotProduct();
            break;
    }
}

void DataflowAnalyzer::executeBinaryOperator(const BinaryOperator *binary, ExecutionContext &context) {
    execute(binary->left(), context);
    execute(binary->right(), context);

    Value *value = dataflow().getValue(binary);
    Value *leftValue = dataflow().getValue(binary->left());
    Value *rightValue = dataflow().getValue(binary->right());

    value->setAbstractValue(apply(binary, leftValue->abstractValue(), rightValue->abstractValue()).merge(value->abstractValue()));

    /* Compute stack offset. */
    switch (binary->operatorKind()) {
        case BinaryOperator::ADD: {
            if (leftValue->isStackOffset()) {
                if (rightValue->abstractValue().isConcrete()) {
                    value->makeStackOffset(leftValue->stackOffset() + rightValue->abstractValue().asConcrete().signedValue());
                } else if (rightValue->abstractValue().isNondeterministic()) {
                    value->makeNotStackOffset();
                }
            }
            if (rightValue->isStackOffset()) {
                if (leftValue->abstractValue().isConcrete()) {
                    value->makeStackOffset(rightValue->stackOffset() + leftValue->abstractValue().asConcrete().signedValue());
                } else if (leftValue->abstractValue().isNondeterministic()) {
                    value->makeNotStackOffset();
                }
            }
            if (leftValue->isNotStackOffset() && rightValue->isNotStackOffset()) {
                value->makeNotStackOffset();
            }
            break;
        }
        case BinaryOperator::SUB: {
            if (leftValue->isStackOffset() && rightValue->abstractValue().isConcrete()) {
                value->makeStackOffset(leftValue->stackOffset() - rightValue->abstractValue().asConcrete().signedValue());
            } else if (leftValue->isNotStackOffset() || rightValue->abstractValue().isNondeterministic()) {
                value->makeNotStackOffset();
            }
            break;
        }
        case BinaryOperator::AND: {
            /* Sometimes used for getting aligned stack pointer values. */
            if (leftValue->isStackOffset() && rightValue->abstractValue().isConcrete()) {
                value->makeStackOffset(leftValue->stackOffset() & rightValue->abstractValue().asConcrete().value());
            } else if (rightValue->isStackOffset() && leftValue->abstractValue().isConcrete()) {
                value->makeStackOffset(rightValue->stackOffset() & leftValue->abstractValue().asConcrete().value());
            } else if ((leftValue->abstractValue().isNondeterministic() && leftValue->isNotStackOffset()) ||
                       (rightValue->abstractValue().isNondeterministic() && rightValue->isNotStackOffset())) {
                value->makeNotStackOffset();
            }
            break;
        }
        default: {
            value->makeNotStackOffset();
            break;
        }
    }

    /* Compute product flag. */
    switch (binary->operatorKind()) {
        case BinaryOperator::MUL:
        case BinaryOperator::SHL:
            value->makeProduct();
            break;
        default:
            value->makeNotProduct();
            break;
    }
}

AbstractValue DataflowAnalyzer::apply(const UnaryOperator *unary, const AbstractValue &a) {
    switch (unary->operatorKind()) {
        case UnaryOperator::NOT:
            return ~a;
        case UnaryOperator::NEGATION:
            return -a;
        case UnaryOperator::SIGN_EXTEND:
            return dflow::AbstractValue(a).signExtend(unary->size());
        case UnaryOperator::ZERO_EXTEND:
            return dflow::AbstractValue(a).zeroExtend(unary->size());
        case UnaryOperator::TRUNCATE:
            return dflow::AbstractValue(a).resize(unary->size());
        default:
            ncWarning("Unknown unary operator kind: %1", unary->operatorKind());
            return dflow::AbstractValue();
    }
}

AbstractValue DataflowAnalyzer::apply(const BinaryOperator *binary, const AbstractValue &a, const AbstractValue &b) {
    switch (binary->operatorKind()) {
        case BinaryOperator::AND:
            return a & b;
        case BinaryOperator::OR:
            return a | b;
        case BinaryOperator::XOR:
            return a ^ b;
        case BinaryOperator::SHL:
            return a << b;
        case BinaryOperator::SHR:
            return a.asUnsigned() >> b;
        case BinaryOperator::SAR:
            return a.asSigned() >> b;
        case BinaryOperator::ADD:
            return a + b;
        case BinaryOperator::SUB:
            return a - b;
        case BinaryOperator::MUL:
            return a * b;
        case BinaryOperator::SIGNED_DIV:
            return a.asSigned() / b;
        case BinaryOperator::SIGNED_REM:
            return a.asSigned() % b;
        case BinaryOperator::UNSIGNED_DIV:
            return a.asUnsigned() / b;
        case BinaryOperator::UNSIGNED_REM:
            return a.asUnsigned() % b;
        case BinaryOperator::EQUAL:
            return a == b;
        case BinaryOperator::SIGNED_LESS:
            return a.asSigned() < b;
        case BinaryOperator::SIGNED_LESS_OR_EQUAL:
            return a.asSigned() <= b;
        case BinaryOperator::UNSIGNED_LESS:
            return a.asUnsigned() < b;
        case BinaryOperator::UNSIGNED_LESS_OR_EQUAL:
            return a.asUnsigned() <= b;
        default:
            ncWarning("Unknown binary operator kind: %1", binary->operatorKind());
            return dflow::AbstractValue();
    }
}

} // namespace dflow
} // namespace ir
} // namespace core
} // namespace nc

/* vim:set et sts=4 sw=4: */
