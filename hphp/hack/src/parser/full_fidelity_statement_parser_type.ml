(**
 * Copyright (c) 2016, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 *)

module type StatementParserType = sig
  type t
  val make : Full_fidelity_lexer.t -> Full_fidelity_syntax_error.t list
    -> Full_fidelity_parser_context.t -> t
  val lexer : t -> Full_fidelity_lexer.t
  val errors : t -> Full_fidelity_syntax_error.t list
  val parse_compound_statement : t -> t * Full_fidelity_minimal_syntax.t
  val parse_statement : t -> t * Full_fidelity_minimal_syntax.t
  val parse_markup_section: t ->
    is_leading_section:bool ->
    t * Full_fidelity_minimal_syntax.t
end
