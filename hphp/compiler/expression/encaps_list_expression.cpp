/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/compiler/expression/encaps_list_expression.h"
#include "hphp/compiler/expression/expression_list.h"
#include "hphp/compiler/expression/binary_op_expression.h"
#include "hphp/compiler/analysis/code_error.h"
#include "hphp/runtime/base/builtin-functions.h"

using namespace HPHP;

///////////////////////////////////////////////////////////////////////////////
// constructors/destructors

EncapsListExpression::EncapsListExpression
(EXPRESSION_CONSTRUCTOR_PARAMETERS, int type, ExpressionListPtr expList)
  : Expression(EXPRESSION_CONSTRUCTOR_PARAMETER_VALUES(EncapsListExpression)),
    m_type(type), m_exps(expList) {
}

ExpressionPtr EncapsListExpression::clone() {
  auto exp = std::make_shared<EncapsListExpression>(*this);
  Expression::deepCopy(exp);
  exp->m_exps = Clone(m_exps);
  return exp;
}

///////////////////////////////////////////////////////////////////////////////
// parser functions

///////////////////////////////////////////////////////////////////////////////
// static analysis functions

ConstructPtr EncapsListExpression::getNthKid(int n) const {
  switch (n) {
    case 0:
      return m_exps;
    default:
      assert(false);
      break;
  }
  return ConstructPtr();
}

int EncapsListExpression::getKidCount() const {
  return 1;
}

void EncapsListExpression::setNthKid(int n, ConstructPtr cp) {
  switch (n) {
    case 0:
      m_exps = dynamic_pointer_cast<ExpressionList>(cp);
      break;
    default:
      assert(false);
      break;
  }
}

void EncapsListExpression::stripConcat() {
  m_exps->stripConcat();
}

ExpressionPtr EncapsListExpression::preOptimize(AnalysisResultConstPtr /*ar*/) {
  if (m_type != '`' && m_type != '\'' && m_exps) {
    int count = m_exps->getCount();
    // turn into cascaded concat
    if (count > 1) {
      auto exp = std::make_shared<BinaryOpExpression>(
        getScope(), getRange(), (*m_exps)[0], (*m_exps)[1], '.'
      );
      for (int i = 2; i < count; i++) {
        exp = std::make_shared<BinaryOpExpression>(
          getScope(), getRange(), exp, (*m_exps)[i], '.'
        );
      }
      return exp;
    }
  }
  return ExpressionPtr();
}

///////////////////////////////////////////////////////////////////////////////
// code generation functions

void EncapsListExpression::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  if (m_type == '`') cg_printf("shell_exec(");

  if (m_exps) {
    for (int i = 0; i < m_exps->getCount(); i++) {
      if (i > 0) cg_printf(" . ");
      (*m_exps)[i]->outputPHP(cg, ar);
    }
  } else {
    cg_printf("''");
  }

  if (m_type == '`') cg_printf(")");
}
