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

#ifndef incl_HPHP_NEW_OBJECT_EXPRESSION_H_
#define incl_HPHP_NEW_OBJECT_EXPRESSION_H_

#include "hphp/compiler/expression/function_call.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

DECLARE_BOOST_TYPES(NewObjectExpression);

struct NewObjectExpression : FunctionCall {
  NewObjectExpression(EXPRESSION_CONSTRUCTOR_PARAMETERS,
                      ExpressionPtr variable, ExpressionListPtr params);

  DECLARE_BASE_EXPRESSION_VIRTUAL_FUNCTIONS;
  void analyzeProgram(AnalysisResultPtr ar) override;

  bool isRefable(bool checkError = false) const override { return checkError; }
private:
  bool m_dynamic;
};

///////////////////////////////////////////////////////////////////////////////
}
#endif // incl_HPHP_NEW_OBJECT_EXPRESSION_H_
