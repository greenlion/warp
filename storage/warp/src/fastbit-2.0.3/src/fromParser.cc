// A Bison parser, made by GNU Bison 3.7.6.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2021 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

// "%code top" blocks.
#line 6 "fromParser.yy"

/** \file Defines the parser for the from clause accepted by FastBit
    IBIS.  The definitions are processed through bison.
*/
#include <iostream>

#line 46 "fromParser.cc"




#include "fromParser.hh"

// Second part of user prologue.
#line 80 "fromParser.yy"

#include "fromLexer.h"

#undef yylex
#define yylex driver.lexer->lex

#line 61 "fromParser.cc"



#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> // FIXME: INFRINGES ON USER NAME SPACE.
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
# if defined __GNUC__ && !defined __EXCEPTIONS
#  define YY_EXCEPTIONS 0
# else
#  define YY_EXCEPTIONS 1
# endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

# ifndef YYLLOC_DEFAULT
#  define YYLLOC_DEFAULT(Current, Rhs, N)                               \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).begin  = YYRHSLOC (Rhs, 1).begin;                   \
          (Current).end    = YYRHSLOC (Rhs, N).end;                     \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).begin = (Current).end = YYRHSLOC (Rhs, 0).end;      \
        }                                                               \
    while (false)
# endif


// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
# define YYCDEBUG if (yydebug_) (*yycdebug_)

# define YY_SYMBOL_PRINT(Title, Symbol)         \
  do {                                          \
    if (yydebug_)                               \
    {                                           \
      *yycdebug_ << Title << ' ';               \
      yy_print_ (*yycdebug_, Symbol);           \
      *yycdebug_ << '\n';                       \
    }                                           \
  } while (false)

# define YY_REDUCE_PRINT(Rule)          \
  do {                                  \
    if (yydebug_)                       \
      yy_reduce_print_ (Rule);          \
  } while (false)

# define YY_STACK_PRINT()               \
  do {                                  \
    if (yydebug_)                       \
      yy_stack_print_ ();                \
  } while (false)

#else // !YYDEBUG

# define YYCDEBUG if (false) std::cerr
# define YY_SYMBOL_PRINT(Title, Symbol)  YY_USE (Symbol)
# define YY_REDUCE_PRINT(Rule)           static_cast<void> (0)
# define YY_STACK_PRINT()                static_cast<void> (0)

#endif // !YYDEBUG

#define yyerrok         (yyerrstatus_ = 0)
#define yyclearin       (yyla.clear ())

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYRECOVERING()  (!!yyerrstatus_)

#line 23 "fromParser.yy"
namespace ibis {
#line 155 "fromParser.cc"

  /// Build a parser object.
  fromParser::fromParser (class ibis::fromClause& driver_yyarg)
#if YYDEBUG
    : yydebug_ (false),
      yycdebug_ (&std::cerr),
#else
    :
#endif
      driver (driver_yyarg)
  {}

  fromParser::~fromParser ()
  {}

  fromParser::syntax_error::~syntax_error () YY_NOEXCEPT YY_NOTHROW
  {}

  /*---------------.
  | symbol kinds.  |
  `---------------*/

  // basic_symbol.
  template <typename Base>
  fromParser::basic_symbol<Base>::basic_symbol (const basic_symbol& that)
    : Base (that)
    , value (that.value)
    , location (that.location)
  {}


  /// Constructor for valueless symbols.
  template <typename Base>
  fromParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, YY_MOVE_REF (location_type) l)
    : Base (t)
    , value ()
    , location (l)
  {}

  template <typename Base>
  fromParser::basic_symbol<Base>::basic_symbol (typename Base::kind_type t, YY_RVREF (semantic_type) v, YY_RVREF (location_type) l)
    : Base (t)
    , value (YY_MOVE (v))
    , location (YY_MOVE (l))
  {}

  template <typename Base>
  fromParser::symbol_kind_type
  fromParser::basic_symbol<Base>::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }

  template <typename Base>
  bool
  fromParser::basic_symbol<Base>::empty () const YY_NOEXCEPT
  {
    return this->kind () == symbol_kind::S_YYEMPTY;
  }

  template <typename Base>
  void
  fromParser::basic_symbol<Base>::move (basic_symbol& s)
  {
    super_type::move (s);
    value = YY_MOVE (s.value);
    location = YY_MOVE (s.location);
  }

  // by_kind.
  fromParser::by_kind::by_kind ()
    : kind_ (symbol_kind::S_YYEMPTY)
  {}

#if 201103L <= YY_CPLUSPLUS
  fromParser::by_kind::by_kind (by_kind&& that)
    : kind_ (that.kind_)
  {
    that.clear ();
  }
#endif

  fromParser::by_kind::by_kind (const by_kind& that)
    : kind_ (that.kind_)
  {}

  fromParser::by_kind::by_kind (token_kind_type t)
    : kind_ (yytranslate_ (t))
  {}

  void
  fromParser::by_kind::clear () YY_NOEXCEPT
  {
    kind_ = symbol_kind::S_YYEMPTY;
  }

  void
  fromParser::by_kind::move (by_kind& that)
  {
    kind_ = that.kind_;
    that.clear ();
  }

  fromParser::symbol_kind_type
  fromParser::by_kind::kind () const YY_NOEXCEPT
  {
    return kind_;
  }

  fromParser::symbol_kind_type
  fromParser::by_kind::type_get () const YY_NOEXCEPT
  {
    return this->kind ();
  }


  // by_state.
  fromParser::by_state::by_state () YY_NOEXCEPT
    : state (empty_state)
  {}

  fromParser::by_state::by_state (const by_state& that) YY_NOEXCEPT
    : state (that.state)
  {}

  void
  fromParser::by_state::clear () YY_NOEXCEPT
  {
    state = empty_state;
  }

  void
  fromParser::by_state::move (by_state& that)
  {
    state = that.state;
    that.clear ();
  }

  fromParser::by_state::by_state (state_type s) YY_NOEXCEPT
    : state (s)
  {}

  fromParser::symbol_kind_type
  fromParser::by_state::kind () const YY_NOEXCEPT
  {
    if (state == empty_state)
      return symbol_kind::S_YYEMPTY;
    else
      return YY_CAST (symbol_kind_type, yystos_[+state]);
  }

  fromParser::stack_symbol_type::stack_symbol_type ()
  {}

  fromParser::stack_symbol_type::stack_symbol_type (YY_RVREF (stack_symbol_type) that)
    : super_type (YY_MOVE (that.state), YY_MOVE (that.value), YY_MOVE (that.location))
  {
#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
  }

  fromParser::stack_symbol_type::stack_symbol_type (state_type s, YY_MOVE_REF (symbol_type) that)
    : super_type (s, YY_MOVE (that.value), YY_MOVE (that.location))
  {
    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
  }

#if YY_CPLUSPLUS < 201103L
  fromParser::stack_symbol_type&
  fromParser::stack_symbol_type::operator= (const stack_symbol_type& that)
  {
    state = that.state;
    value = that.value;
    location = that.location;
    return *this;
  }

  fromParser::stack_symbol_type&
  fromParser::stack_symbol_type::operator= (stack_symbol_type& that)
  {
    state = that.state;
    value = that.value;
    location = that.location;
    // that is emptied.
    that.state = empty_state;
    return *this;
  }
#endif

  template <typename Base>
  void
  fromParser::yy_destroy_ (const char* yymsg, basic_symbol<Base>& yysym) const
  {
    if (yymsg)
      YY_SYMBOL_PRINT (yymsg, yysym);

    // User destructor.
    switch (yysym.kind ())
    {
      case symbol_kind::S_NOUNSTR: // "name"
#line 77 "fromParser.yy"
                    { delete (yysym.value.stringVal); }
#line 361 "fromParser.cc"
        break;

      case symbol_kind::S_compRange2: // compRange2
#line 78 "fromParser.yy"
                    { delete (yysym.value.fromNode); }
#line 367 "fromParser.cc"
        break;

      case symbol_kind::S_compRange3: // compRange3
#line 78 "fromParser.yy"
                    { delete (yysym.value.fromNode); }
#line 373 "fromParser.cc"
        break;

      case symbol_kind::S_mathExpr: // mathExpr
#line 78 "fromParser.yy"
                    { delete (yysym.value.fromNode); }
#line 379 "fromParser.cc"
        break;

      default:
        break;
    }
  }

#if YYDEBUG
  template <typename Base>
  void
  fromParser::yy_print_ (std::ostream& yyo, const basic_symbol<Base>& yysym) const
  {
    std::ostream& yyoutput = yyo;
    YY_USE (yyoutput);
    if (yysym.empty ())
      yyo << "empty symbol";
    else
      {
        symbol_kind_type yykind = yysym.kind ();
        yyo << (yykind < YYNTOKENS ? "token" : "nterm")
            << ' ' << yysym.name () << " ("
            << yysym.location << ": ";
        YY_USE (yykind);
        yyo << ')';
      }
  }
#endif

  void
  fromParser::yypush_ (const char* m, YY_MOVE_REF (stack_symbol_type) sym)
  {
    if (m)
      YY_SYMBOL_PRINT (m, sym);
    yystack_.push (YY_MOVE (sym));
  }

  void
  fromParser::yypush_ (const char* m, state_type s, YY_MOVE_REF (symbol_type) sym)
  {
#if 201103L <= YY_CPLUSPLUS
    yypush_ (m, stack_symbol_type (s, std::move (sym)));
#else
    stack_symbol_type ss (s, sym);
    yypush_ (m, ss);
#endif
  }

  void
  fromParser::yypop_ (int n)
  {
    yystack_.pop (n);
  }

#if YYDEBUG
  std::ostream&
  fromParser::debug_stream () const
  {
    return *yycdebug_;
  }

  void
  fromParser::set_debug_stream (std::ostream& o)
  {
    yycdebug_ = &o;
  }


  fromParser::debug_level_type
  fromParser::debug_level () const
  {
    return yydebug_;
  }

  void
  fromParser::set_debug_level (debug_level_type l)
  {
    yydebug_ = l;
  }
#endif // YYDEBUG

  fromParser::state_type
  fromParser::yy_lr_goto_state_ (state_type yystate, int yysym)
  {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
      return yytable_[yyr];
    else
      return yydefgoto_[yysym - YYNTOKENS];
  }

  bool
  fromParser::yy_pact_value_is_default_ (int yyvalue)
  {
    return yyvalue == yypact_ninf_;
  }

  bool
  fromParser::yy_table_value_is_error_ (int yyvalue)
  {
    return yyvalue == yytable_ninf_;
  }

  int
  fromParser::operator() ()
  {
    return parse ();
  }

  int
  fromParser::parse ()
  {
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// The return value of parse ().
    int yyresult;

#if YY_EXCEPTIONS
    try
#endif // YY_EXCEPTIONS
      {
    YYCDEBUG << "Starting parse\n";


    // User initialization code.
#line 28 "fromParser.yy"
{ // initialize location object
    yyla.location.begin.filename = yyla.location.end.filename = &(driver.clause_);
}

#line 521 "fromParser.cc"


    /* Initialize the stack.  The initial state will be set in
       yynewstate, since the latter expects the semantical and the
       location values to have been already stored, initialize these
       stacks with a primary value.  */
    yystack_.clear ();
    yypush_ (YY_NULLPTR, 0, YY_MOVE (yyla));

  /*-----------------------------------------------.
  | yynewstate -- push a new symbol on the stack.  |
  `-----------------------------------------------*/
  yynewstate:
    YYCDEBUG << "Entering state " << int (yystack_[0].state) << '\n';
    YY_STACK_PRINT ();

    // Accept?
    if (yystack_[0].state == yyfinal_)
      YYACCEPT;

    goto yybackup;


  /*-----------.
  | yybackup.  |
  `-----------*/
  yybackup:
    // Try to take a decision without lookahead.
    yyn = yypact_[+yystack_[0].state];
    if (yy_pact_value_is_default_ (yyn))
      goto yydefault;

    // Read a lookahead token.
    if (yyla.empty ())
      {
        YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
        try
#endif // YY_EXCEPTIONS
          {
            yyla.kind_ = yytranslate_ (yylex (&yyla.value, &yyla.location));
          }
#if YY_EXCEPTIONS
        catch (const syntax_error& yyexc)
          {
            YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
            error (yyexc);
            goto yyerrlab1;
          }
#endif // YY_EXCEPTIONS
      }
    YY_SYMBOL_PRINT ("Next token is", yyla);

    if (yyla.kind () == symbol_kind::S_YYerror)
    {
      // The scanner already issued an error message, process directly
      // to error recovery.  But do not keep the error token as
      // lookahead, it is too special and may lead us to an endless
      // loop in error recovery. */
      yyla.kind_ = symbol_kind::S_YYUNDEF;
      goto yyerrlab1;
    }

    /* If the proper action on seeing token YYLA.TYPE is to reduce or
       to detect an error, take that action.  */
    yyn += yyla.kind ();
    if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind ())
      {
        goto yydefault;
      }

    // Reduce or error.
    yyn = yytable_[yyn];
    if (yyn <= 0)
      {
        if (yy_table_value_is_error_ (yyn))
          goto yyerrlab;
        yyn = -yyn;
        goto yyreduce;
      }

    // Count tokens shifted since error; after three, turn off error status.
    if (yyerrstatus_)
      --yyerrstatus_;

    // Shift the lookahead token.
    yypush_ ("Shifting", state_type (yyn), YY_MOVE (yyla));
    goto yynewstate;


  /*-----------------------------------------------------------.
  | yydefault -- do the default action for the current state.  |
  `-----------------------------------------------------------*/
  yydefault:
    yyn = yydefact_[+yystack_[0].state];
    if (yyn == 0)
      goto yyerrlab;
    goto yyreduce;


  /*-----------------------------.
  | yyreduce -- do a reduction.  |
  `-----------------------------*/
  yyreduce:
    yylen = yyr2_[yyn];
    {
      stack_symbol_type yylhs;
      yylhs.state = yy_lr_goto_state_ (yystack_[yylen].state, yyr1_[yyn]);
      /* If YYLEN is nonzero, implement the default value of the
         action: '$$ = $1'.  Otherwise, use the top of the stack.

         Otherwise, the following line sets YYLHS.VALUE to garbage.
         This behavior is undocumented and Bison users should not rely
         upon it.  */
      if (yylen)
        yylhs.value = yystack_[yylen - 1].value;
      else
        yylhs.value = yystack_[0].value;

      // Default location.
      {
        stack_type::slice range (yystack_, yylen);
        YYLLOC_DEFAULT (yylhs.location, range, yylen);
        yyerror_range[1].location = yylhs.location;
      }

      // Perform the reduction.
      YY_REDUCE_PRINT (yyn);
#if YY_EXCEPTIONS
      try
#endif // YY_EXCEPTIONS
        {
          switch (yyn)
            {
  case 4: // fterm: "name" ','
#line 89 "fromParser.yy"
                   {
    driver.names_.push_back(*(yystack_[1].value.stringVal));
    driver.aliases_.push_back("");
}
#line 662 "fromParser.cc"
    break;

  case 5: // fterm: "name" "end of input"
#line 93 "fromParser.yy"
              {
    driver.names_.push_back(*(yystack_[1].value.stringVal));
    driver.aliases_.push_back("");
}
#line 671 "fromParser.cc"
    break;

  case 6: // fterm: "name" "name" ','
#line 97 "fromParser.yy"
                      {
    driver.names_.push_back(*(yystack_[2].value.stringVal));
    driver.aliases_.push_back(*(yystack_[1].value.stringVal));
}
#line 680 "fromParser.cc"
    break;

  case 7: // fterm: "name" "name" "end of input"
#line 101 "fromParser.yy"
                      {
    driver.names_.push_back(*(yystack_[2].value.stringVal));
    driver.aliases_.push_back(*(yystack_[1].value.stringVal));
}
#line 689 "fromParser.cc"
    break;

  case 8: // fterm: "name" "as" "name" ','
#line 104 "fromParser.yy"
                            {
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back(*(yystack_[1].value.stringVal));
}
#line 698 "fromParser.cc"
    break;

  case 9: // fterm: "name" "as" "name" "end of input"
#line 108 "fromParser.yy"
                           {
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back(*(yystack_[1].value.stringVal));
}
#line 707 "fromParser.cc"
    break;

  case 10: // fterm: "name" "join" "name" "end of input"
#line 112 "fromParser.yy"
                             {
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[1].value.stringVal));
    driver.aliases_.push_back("");
}
#line 718 "fromParser.cc"
    break;

  case 11: // fterm: "name" "as" "name" "join" "name" "end of input"
#line 118 "fromParser.yy"
                                          {
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    driver.names_.push_back(*(yystack_[1].value.stringVal));
    driver.aliases_.push_back("");
}
#line 729 "fromParser.cc"
    break;

  case 12: // fterm: "name" "as" "name" "join" "name" "as" "name" "end of input"
#line 124 "fromParser.yy"
                                                       {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back(*(yystack_[1].value.stringVal));
}
#line 740 "fromParser.cc"
    break;

  case 13: // fterm: "name" "join" "name" "as" "name" "end of input"
#line 130 "fromParser.yy"
                                          {
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back(*(yystack_[1].value.stringVal));
}
#line 751 "fromParser.cc"
    break;

  case 14: // fterm: "name" "join" "name" "using" "name" "end of input"
#line 136 "fromParser.yy"
                                             {
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    std::string nm1 = *(yystack_[5].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 771 "fromParser.cc"
    break;

  case 15: // fterm: "name" "as" "name" "join" "name" "using" "name" "end of input"
#line 151 "fromParser.yy"
                                                          {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    std::string nm1 = *(yystack_[5].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 791 "fromParser.cc"
    break;

  case 16: // fterm: "name" "as" "name" "join" "name" "as" "name" "using" "name" "end of input"
#line 166 "fromParser.yy"
                                                                       {
    driver.names_.push_back(*(yystack_[9].value.stringVal));
    driver.aliases_.push_back(*(yystack_[7].value.stringVal));
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    std::string nm1 = *(yystack_[7].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 811 "fromParser.cc"
    break;

  case 17: // fterm: "name" "join" "name" "as" "name" "using" "name" "end of input"
#line 181 "fromParser.yy"
                                                          {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    std::string nm1 = *(yystack_[7].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 831 "fromParser.cc"
    break;

  case 18: // fterm: "name" "name" "join" "name" "using" "name" "end of input"
#line 196 "fromParser.yy"
                                                     {
    driver.names_.push_back(*(yystack_[6].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    std::string nm1 = *(yystack_[5].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 851 "fromParser.cc"
    break;

  case 19: // fterm: "name" "name" "join" "name" "name" "using" "name" "end of input"
#line 211 "fromParser.yy"
                                                             {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[6].value.stringVal));
    driver.names_.push_back(*(yystack_[4].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    std::string nm1 = *(yystack_[6].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 871 "fromParser.cc"
    break;

  case 20: // fterm: "name" "join" "name" "name" "using" "name" "end of input"
#line 226 "fromParser.yy"
                                                     {
    driver.names_.push_back(*(yystack_[6].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[4].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    std::string nm1 = *(yystack_[6].value.stringVal);
    std::string nm2 = *(yystack_[3].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[1].value.stringVal);
    nm2 += *(yystack_[1].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 891 "fromParser.cc"
    break;

  case 21: // fterm: "name" "join" "name" "using" '(' "name" ')' "end of input"
#line 241 "fromParser.yy"
                                                     {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back("");
    std::string nm1 = *(yystack_[7].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 911 "fromParser.cc"
    break;

  case 22: // fterm: "name" "as" "name" "join" "name" "using" '(' "name" ')' "end of input"
#line 256 "fromParser.yy"
                                                                  {
    driver.names_.push_back(*(yystack_[9].value.stringVal));
    driver.aliases_.push_back(*(yystack_[7].value.stringVal));
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back("");
    std::string nm1 = *(yystack_[7].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 931 "fromParser.cc"
    break;

  case 23: // fterm: "name" "as" "name" "join" "name" "as" "name" "using" '(' "name" ')' "end of input"
#line 271 "fromParser.yy"
                                                                               {
    driver.names_.push_back(*(yystack_[11].value.stringVal));
    driver.aliases_.push_back(*(yystack_[9].value.stringVal));
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    std::string nm1 = *(yystack_[9].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 951 "fromParser.cc"
    break;

  case 24: // fterm: "name" "join" "name" "as" "name" "using" '(' "name" ')' "end of input"
#line 286 "fromParser.yy"
                                                                  {
    driver.names_.push_back(*(yystack_[9].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    std::string nm1 = *(yystack_[9].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 971 "fromParser.cc"
    break;

  case 25: // fterm: "name" "name" "join" "name" "using" '(' "name" ')' "end of input"
#line 301 "fromParser.yy"
                                                             {
    driver.names_.push_back(*(yystack_[8].value.stringVal));
    driver.aliases_.push_back(*(yystack_[7].value.stringVal));
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back("");
    std::string nm1 = *(yystack_[7].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 991 "fromParser.cc"
    break;

  case 26: // fterm: "name" "name" "join" "name" "name" "using" '(' "name" ')' "end of input"
#line 316 "fromParser.yy"
                                                                     {
    driver.names_.push_back(*(yystack_[9].value.stringVal));
    driver.aliases_.push_back(*(yystack_[8].value.stringVal));
    driver.names_.push_back(*(yystack_[6].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    std::string nm1 = *(yystack_[8].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 1011 "fromParser.cc"
    break;

  case 27: // fterm: "name" "join" "name" "name" "using" '(' "name" ')' "end of input"
#line 331 "fromParser.yy"
                                                             {
    driver.names_.push_back(*(yystack_[8].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[6].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    std::string nm1 = *(yystack_[8].value.stringVal);
    std::string nm2 = *(yystack_[5].value.stringVal);
    nm1 += '.';
    nm2 += '.';
    nm1 += *(yystack_[2].value.stringVal);
    nm2 += *(yystack_[2].value.stringVal);
    ibis::math::variable* var1 = new ibis::math::variable(nm1.c_str());
    ibis::math::variable* var2 = new ibis::math::variable(nm2.c_str());
    driver.jcond_ = new ibis::compRange(var1, ibis::qExpr::OP_EQ, var2);
}
#line 1031 "fromParser.cc"
    break;

  case 28: // fterm: "name" "join" "name" "on" compRange "end of input"
#line 346 "fromParser.yy"
                                            {
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1043 "fromParser.cc"
    break;

  case 29: // fterm: "name" "as" "name" "join" "name" "on" compRange "end of input"
#line 353 "fromParser.yy"
                                                         {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1055 "fromParser.cc"
    break;

  case 30: // fterm: "name" "as" "name" "join" "name" "as" "name" "on" compRange "end of input"
#line 360 "fromParser.yy"
                                                                      {
    driver.names_.push_back(*(yystack_[9].value.stringVal));
    driver.aliases_.push_back(*(yystack_[7].value.stringVal));
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1067 "fromParser.cc"
    break;

  case 31: // fterm: "name" "join" "name" "as" "name" "on" compRange "end of input"
#line 367 "fromParser.yy"
                                                         {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[5].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1079 "fromParser.cc"
    break;

  case 32: // fterm: "name" "name" "join" "name" "on" compRange "end of input"
#line 374 "fromParser.yy"
                                                    {
    driver.names_.push_back(*(yystack_[6].value.stringVal));
    driver.aliases_.push_back(*(yystack_[5].value.stringVal));
    driver.names_.push_back(*(yystack_[3].value.stringVal));
    driver.aliases_.push_back("");
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1091 "fromParser.cc"
    break;

  case 33: // fterm: "name" "name" "join" "name" "name" "on" compRange "end of input"
#line 381 "fromParser.yy"
                                                            {
    driver.names_.push_back(*(yystack_[7].value.stringVal));
    driver.aliases_.push_back(*(yystack_[6].value.stringVal));
    driver.names_.push_back(*(yystack_[4].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1103 "fromParser.cc"
    break;

  case 34: // fterm: "name" "join" "name" "name" "on" compRange "end of input"
#line 388 "fromParser.yy"
                                                    {
    driver.names_.push_back(*(yystack_[6].value.stringVal));
    driver.aliases_.push_back("");
    driver.names_.push_back(*(yystack_[4].value.stringVal));
    driver.aliases_.push_back(*(yystack_[3].value.stringVal));
    driver.jcond_ = dynamic_cast<ibis::compRange*>((yystack_[1].value.fromNode));
}
#line 1115 "fromParser.cc"
    break;

  case 35: // compRange: compRange2
#line 397 "fromParser.yy"
           { (yylhs.value.fromNode) = (yystack_[0].value.fromNode); }
#line 1121 "fromParser.cc"
    break;

  case 36: // compRange: compRange3
#line 397 "fromParser.yy"
                        { (yylhs.value.fromNode) = (yystack_[0].value.fromNode); }
#line 1127 "fromParser.cc"
    break;

  case 37: // compRange2: mathExpr "==" mathExpr
#line 399 "fromParser.yy"
                       {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " = "
	<< *me2;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_EQ, me2);
}
#line 1142 "fromParser.cc"
    break;

  case 38: // compRange2: mathExpr "!=" mathExpr
#line 409 "fromParser.yy"
                          {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " != "
	<< *me2;
#endif
    (yylhs.value.fromNode) = new ibis::qExpr(ibis::qExpr::LOGICAL_NOT);
    (yylhs.value.fromNode)->setLeft(new ibis::compRange(me1, ibis::qExpr::OP_EQ, me2));
}
#line 1158 "fromParser.cc"
    break;

  case 39: // compRange2: mathExpr "<" mathExpr
#line 420 "fromParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " < "
	<< *me2;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_LT, me2);
}
#line 1173 "fromParser.cc"
    break;

  case 40: // compRange2: mathExpr "<=" mathExpr
#line 430 "fromParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " <= "
	<< *me2;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_LE, me2);
}
#line 1188 "fromParser.cc"
    break;

  case 41: // compRange2: mathExpr ">" mathExpr
#line 440 "fromParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " > "
	<< *me2;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_GT, me2);
}
#line 1203 "fromParser.cc"
    break;

  case 42: // compRange2: mathExpr ">=" mathExpr
#line 450 "fromParser.yy"
                         {
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " >= "
	<< *me2;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_GE, me2);
}
#line 1218 "fromParser.cc"
    break;

  case 43: // compRange3: mathExpr "<" mathExpr "<" mathExpr
#line 463 "fromParser.yy"
                                     {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " < "
	<< *me2 << " < " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LT, me3);
}
#line 1235 "fromParser.cc"
    break;

  case 44: // compRange3: mathExpr "<" mathExpr "<=" mathExpr
#line 475 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " < "
	<< *me2 << " <= " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LE, me3);
}
#line 1252 "fromParser.cc"
    break;

  case 45: // compRange3: mathExpr "<=" mathExpr "<" mathExpr
#line 487 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " <= "
	<< *me2 << " < " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LT, me3);
}
#line 1269 "fromParser.cc"
    break;

  case 46: // compRange3: mathExpr "<=" mathExpr "<=" mathExpr
#line 499 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " <= "
	<< *me2 << " <= " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me1, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LE, me3);
}
#line 1286 "fromParser.cc"
    break;

  case 47: // compRange3: mathExpr ">" mathExpr ">" mathExpr
#line 511 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " > "
	<< *me2 << " > " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me3, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LT, me1);
}
#line 1303 "fromParser.cc"
    break;

  case 48: // compRange3: mathExpr ">" mathExpr ">=" mathExpr
#line 523 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " > "
	<< *me2 << " >= " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me3, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LT, me1);
}
#line 1320 "fromParser.cc"
    break;

  case 49: // compRange3: mathExpr ">=" mathExpr ">" mathExpr
#line 535 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " >= "
	<< *me2 << " > " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me3, ibis::qExpr::OP_LT, me2,
			     ibis::qExpr::OP_LE, me1);
}
#line 1337 "fromParser.cc"
    break;

  case 50: // compRange3: mathExpr ">=" mathExpr ">=" mathExpr
#line 547 "fromParser.yy"
                                       {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " >= "
	<< *me2 << " >= " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me3, ibis::qExpr::OP_LE, me2,
			     ibis::qExpr::OP_LE, me1);
}
#line 1354 "fromParser.cc"
    break;

  case 51: // compRange3: mathExpr "between" mathExpr "and" mathExpr
#line 559 "fromParser.yy"
                                             {
    ibis::math::term *me3 = static_cast<ibis::math::term*>((yystack_[0].value.fromNode));
    ibis::math::term *me2 = static_cast<ibis::math::term*>((yystack_[2].value.fromNode));
    ibis::math::term *me1 = static_cast<ibis::math::term*>((yystack_[4].value.fromNode));
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *me1 << " BETWEEN "
	<< *me2 << " AND " << *me3;
#endif
    (yylhs.value.fromNode) = new ibis::compRange(me2, ibis::qExpr::OP_LE, me1,
			     ibis::qExpr::OP_LE, me3);
}
#line 1371 "fromParser.cc"
    break;

  case 52: // mathExpr: mathExpr "+" mathExpr
#line 574 "fromParser.yy"
                        {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " + " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::PLUS);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1388 "fromParser.cc"
    break;

  case 53: // mathExpr: mathExpr "-" mathExpr
#line 586 "fromParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " - " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::MINUS);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1405 "fromParser.cc"
    break;

  case 54: // mathExpr: mathExpr "*" mathExpr
#line 598 "fromParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " * " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::MULTIPLY);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1422 "fromParser.cc"
    break;

  case 55: // mathExpr: mathExpr "/" mathExpr
#line 610 "fromParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " / " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::DIVIDE);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1439 "fromParser.cc"
    break;

  case 56: // mathExpr: mathExpr "%" mathExpr
#line 622 "fromParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " % " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::REMAINDER);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1456 "fromParser.cc"
    break;

  case 57: // mathExpr: mathExpr "**" mathExpr
#line 634 "fromParser.yy"
                          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " ^ " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::POWER);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1473 "fromParser.cc"
    break;

  case 58: // mathExpr: mathExpr "&" mathExpr
#line 646 "fromParser.yy"
                             {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " & " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::BITAND);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1490 "fromParser.cc"
    break;

  case 59: // mathExpr: mathExpr "|" mathExpr
#line 658 "fromParser.yy"
                            {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[2].value.fromNode)
	<< " | " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::BITOR);
    opr->setRight((yystack_[0].value.fromNode));
    opr->setLeft((yystack_[2].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1507 "fromParser.cc"
    break;

  case 60: // mathExpr: "name" '(' mathExpr ')'
#line 670 "fromParser.yy"
                           {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[3].value.stringVal) << "("
	<< *(yystack_[1].value.fromNode) << ")";
#endif
    ibis::math::stdFunction1 *fun =
	new ibis::math::stdFunction1((yystack_[3].value.stringVal)->c_str());
    delete (yystack_[3].value.stringVal);
    fun->setLeft((yystack_[1].value.fromNode));
    (yylhs.value.fromNode) = fun;
}
#line 1524 "fromParser.cc"
    break;

  case 61: // mathExpr: "name" '(' mathExpr ',' mathExpr ')'
#line 682 "fromParser.yy"
                                        {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- " << *(yystack_[5].value.stringVal) << "("
	<< *(yystack_[3].value.fromNode) << ", " << *(yystack_[1].value.fromNode) << ")";
#endif
    ibis::math::stdFunction2 *fun =
	new ibis::math::stdFunction2((yystack_[5].value.stringVal)->c_str());
    fun->setRight((yystack_[1].value.fromNode));
    fun->setLeft((yystack_[3].value.fromNode));
    (yylhs.value.fromNode) = fun;
    delete (yystack_[5].value.stringVal);
}
#line 1542 "fromParser.cc"
    break;

  case 62: // mathExpr: "-" mathExpr
#line 695 "fromParser.yy"
                               {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " parsing -- - " << *(yystack_[0].value.fromNode);
#endif
    ibis::math::bediener *opr =
	new ibis::math::bediener(ibis::math::NEGATE);
    opr->setRight((yystack_[0].value.fromNode));
    (yylhs.value.fromNode) = opr;
}
#line 1557 "fromParser.cc"
    break;

  case 63: // mathExpr: "+" mathExpr
#line 705 "fromParser.yy"
                             {
    (yylhs.value.fromNode) = (yystack_[0].value.fromNode);
}
#line 1565 "fromParser.cc"
    break;

  case 64: // mathExpr: '(' mathExpr ')'
#line 708 "fromParser.yy"
                   {
    (yylhs.value.fromNode) = (yystack_[1].value.fromNode);
}
#line 1573 "fromParser.cc"
    break;

  case 65: // mathExpr: "name"
#line 711 "fromParser.yy"
          {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a variable name " << *(yystack_[0].value.stringVal);
#endif
    (yylhs.value.fromNode) = new ibis::math::variable((yystack_[0].value.stringVal)->c_str());
    delete (yystack_[0].value.stringVal);
}
#line 1586 "fromParser.cc"
    break;

  case 66: // mathExpr: "numerical value"
#line 719 "fromParser.yy"
         {
#if defined(DEBUG) && DEBUG + 0 > 1
    LOGGER(ibis::gVerbose >= 0)
	<< __FILE__ << ":" << __LINE__ << " got a number " << (yystack_[0].value.doubleVal);
#endif
    (yylhs.value.fromNode) = new ibis::math::number((yystack_[0].value.doubleVal));
}
#line 1598 "fromParser.cc"
    break;


#line 1602 "fromParser.cc"

            default:
              break;
            }
        }
#if YY_EXCEPTIONS
      catch (const syntax_error& yyexc)
        {
          YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
          error (yyexc);
          YYERROR;
        }
#endif // YY_EXCEPTIONS
      YY_SYMBOL_PRINT ("-> $$ =", yylhs);
      yypop_ (yylen);
      yylen = 0;

      // Shift the result of the reduction.
      yypush_ (YY_NULLPTR, YY_MOVE (yylhs));
    }
    goto yynewstate;


  /*--------------------------------------.
  | yyerrlab -- here on detecting error.  |
  `--------------------------------------*/
  yyerrlab:
    // If not already recovering from an error, report this error.
    if (!yyerrstatus_)
      {
        ++yynerrs_;
        context yyctx (*this, yyla);
        std::string msg = yysyntax_error_ (yyctx);
        error (yyla.location, YY_MOVE (msg));
      }


    yyerror_range[1].location = yyla.location;
    if (yyerrstatus_ == 3)
      {
        /* If just tried and failed to reuse lookahead token after an
           error, discard it.  */

        // Return failure if at end of input.
        if (yyla.kind () == symbol_kind::S_YYEOF)
          YYABORT;
        else if (!yyla.empty ())
          {
            yy_destroy_ ("Error: discarding", yyla);
            yyla.clear ();
          }
      }

    // Else will try to reuse lookahead token after shifting the error token.
    goto yyerrlab1;


  /*---------------------------------------------------.
  | yyerrorlab -- error raised explicitly by YYERROR.  |
  `---------------------------------------------------*/
  yyerrorlab:
    /* Pacify compilers when the user code never invokes YYERROR and
       the label yyerrorlab therefore never appears in user code.  */
    if (false)
      YYERROR;

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYERROR.  */
    yypop_ (yylen);
    yylen = 0;
    YY_STACK_PRINT ();
    goto yyerrlab1;


  /*-------------------------------------------------------------.
  | yyerrlab1 -- common code for both syntax error and YYERROR.  |
  `-------------------------------------------------------------*/
  yyerrlab1:
    yyerrstatus_ = 3;   // Each real token shifted decrements this.
    // Pop stack until we find a state that shifts the error token.
    for (;;)
      {
        yyn = yypact_[+yystack_[0].state];
        if (!yy_pact_value_is_default_ (yyn))
          {
            yyn += symbol_kind::S_YYerror;
            if (0 <= yyn && yyn <= yylast_
                && yycheck_[yyn] == symbol_kind::S_YYerror)
              {
                yyn = yytable_[yyn];
                if (0 < yyn)
                  break;
              }
          }

        // Pop the current state because it cannot handle the error token.
        if (yystack_.size () == 1)
          YYABORT;

        yyerror_range[1].location = yystack_[0].location;
        yy_destroy_ ("Error: popping", yystack_[0]);
        yypop_ ();
        YY_STACK_PRINT ();
      }
    {
      stack_symbol_type error_token;

      yyerror_range[2].location = yyla.location;
      YYLLOC_DEFAULT (error_token.location, yyerror_range, 2);

      // Shift the error token.
      error_token.state = state_type (yyn);
      yypush_ ("Shifting", YY_MOVE (error_token));
    }
    goto yynewstate;


  /*-------------------------------------.
  | yyacceptlab -- YYACCEPT comes here.  |
  `-------------------------------------*/
  yyacceptlab:
    yyresult = 0;
    goto yyreturn;


  /*-----------------------------------.
  | yyabortlab -- YYABORT comes here.  |
  `-----------------------------------*/
  yyabortlab:
    yyresult = 1;
    goto yyreturn;


  /*-----------------------------------------------------.
  | yyreturn -- parsing is finished, return the result.  |
  `-----------------------------------------------------*/
  yyreturn:
    if (!yyla.empty ())
      yy_destroy_ ("Cleanup: discarding lookahead", yyla);

    /* Do not reclaim the symbols of the rule whose action triggered
       this YYABORT or YYACCEPT.  */
    yypop_ (yylen);
    YY_STACK_PRINT ();
    while (1 < yystack_.size ())
      {
        yy_destroy_ ("Cleanup: popping", yystack_[0]);
        yypop_ ();
      }

    return yyresult;
  }
#if YY_EXCEPTIONS
    catch (...)
      {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
        // Do not try to display the values of the reclaimed symbols,
        // as their printers might throw an exception.
        if (!yyla.empty ())
          yy_destroy_ (YY_NULLPTR, yyla);

        while (1 < yystack_.size ())
          {
            yy_destroy_ (YY_NULLPTR, yystack_[0]);
            yypop_ ();
          }
        throw;
      }
#endif // YY_EXCEPTIONS
  }

  void
  fromParser::error (const syntax_error& yyexc)
  {
    error (yyexc.location, yyexc.what ());
  }

  /* Return YYSTR after stripping away unnecessary quotes and
     backslashes, so that it's suitable for yyerror.  The heuristic is
     that double-quoting is unnecessary unless the string contains an
     apostrophe, a comma, or backslash (other than backslash-backslash).
     YYSTR is taken from yytname.  */
  std::string
  fromParser::yytnamerr_ (const char *yystr)
  {
    if (*yystr == '"')
      {
        std::string yyr;
        char const *yyp = yystr;

        for (;;)
          switch (*++yyp)
            {
            case '\'':
            case ',':
              goto do_not_strip_quotes;

            case '\\':
              if (*++yyp != '\\')
                goto do_not_strip_quotes;
              else
                goto append;

            append:
            default:
              yyr += *yyp;
              break;

            case '"':
              return yyr;
            }
      do_not_strip_quotes: ;
      }

    return yystr;
  }

  std::string
  fromParser::symbol_name (symbol_kind_type yysymbol)
  {
    return yytnamerr_ (yytname_[yysymbol]);
  }



  // fromParser::context.
  fromParser::context::context (const fromParser& yyparser, const symbol_type& yyla)
    : yyparser_ (yyparser)
    , yyla_ (yyla)
  {}

  int
  fromParser::context::expected_tokens (symbol_kind_type yyarg[], int yyargn) const
  {
    // Actual number of expected tokens
    int yycount = 0;

    int yyn = yypact_[+yyparser_.yystack_[0].state];
    if (!yy_pact_value_is_default_ (yyn))
      {
        /* Start YYX at -YYN if negative to avoid negative indexes in
           YYCHECK.  In other words, skip the first -YYN actions for
           this state because they are default actions.  */
        int yyxbegin = yyn < 0 ? -yyn : 0;
        // Stay within bounds of both yycheck and yytname.
        int yychecklim = yylast_ - yyn + 1;
        int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
        for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
          if (yycheck_[yyx + yyn] == yyx && yyx != symbol_kind::S_YYerror
              && !yy_table_value_is_error_ (yytable_[yyx + yyn]))
            {
              if (!yyarg)
                ++yycount;
              else if (yycount == yyargn)
                return 0;
              else
                yyarg[yycount++] = YY_CAST (symbol_kind_type, yyx);
            }
      }

    if (yyarg && yycount == 0 && 0 < yyargn)
      yyarg[0] = symbol_kind::S_YYEMPTY;
    return yycount;
  }



  int
  fromParser::yy_syntax_error_arguments_ (const context& yyctx,
                                                 symbol_kind_type yyarg[], int yyargn) const
  {
    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yyla) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state merging
         (from LALR or IELR) and default reductions corrupt the expected
         token list.  However, the list is correct for canonical LR with
         one exception: it will still contain any token that will not be
         accepted due to an error action in a later state.
    */

    if (!yyctx.lookahead ().empty ())
      {
        if (yyarg)
          yyarg[0] = yyctx.token ();
        int yyn = yyctx.expected_tokens (yyarg ? yyarg + 1 : yyarg, yyargn - 1);
        return yyn + 1;
      }
    return 0;
  }

  // Generate an error message.
  std::string
  fromParser::yysyntax_error_ (const context& yyctx) const
  {
    // Its maximum.
    enum { YYARGS_MAX = 5 };
    // Arguments of yyformat.
    symbol_kind_type yyarg[YYARGS_MAX];
    int yycount = yy_syntax_error_arguments_ (yyctx, yyarg, YYARGS_MAX);

    char const* yyformat = YY_NULLPTR;
    switch (yycount)
      {
#define YYCASE_(N, S)                         \
        case N:                               \
          yyformat = S;                       \
        break
      default: // Avoid compiler warnings.
        YYCASE_ (0, YY_("syntax error"));
        YYCASE_ (1, YY_("syntax error, unexpected %s"));
        YYCASE_ (2, YY_("syntax error, unexpected %s, expecting %s"));
        YYCASE_ (3, YY_("syntax error, unexpected %s, expecting %s or %s"));
        YYCASE_ (4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
        YYCASE_ (5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
      }

    std::string yyres;
    // Argument number.
    std::ptrdiff_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
      if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount)
        {
          yyres += symbol_name (yyarg[yyi++]);
          ++yyp;
        }
      else
        yyres += *yyp;
    return yyres;
  }


  const signed char fromParser::yypact_ninf_ = -36;

  const signed char fromParser::yytable_ninf_ = -1;

  const short
  fromParser::yypact_[] =
  {
     -18,    21,    11,   -18,   -36,   -12,    -6,     2,   -36,   -36,
     -36,    44,    47,   -36,    -1,   -36,   -36,    42,   -36,   -36,
      43,    -9,    39,    91,    48,    16,    64,    -9,    -9,   -36,
      85,    -9,   122,   -36,   -36,    72,   125,    88,    -9,    84,
      -9,   100,   154,   -36,   119,    -9,   167,   -36,    -9,   190,
     126,   126,    -9,   155,   -36,    -9,    -9,    -9,    -9,    -9,
      -9,    -9,    -9,    -9,    -9,    -9,    -9,    -9,    -9,    -9,
     -36,   165,   161,   212,   170,   213,   222,   199,    -9,   194,
     211,   224,   225,   202,   227,   228,   205,   120,   -36,    40,
      98,   134,   112,   147,   181,   181,   188,    56,   159,   159,
     126,   126,   126,   126,   230,   -36,   -36,   204,   -36,   -36,
     206,   232,   234,   214,   -36,    -9,   195,   -36,   -36,   208,
     -36,   -36,   209,    -9,   -36,    -9,    -9,    -9,    -9,    -9,
      -9,    -9,    -9,    -9,   -36,   237,   239,   -36,   -36,   215,
     240,   241,   219,   244,   245,   168,   181,   181,   181,   181,
     181,   181,   181,   181,   181,   -36,   -36,   246,   -36,   -36,
     220,   -36,   -36,   -36,   -36,   248,   -36
  };

  const signed char
  fromParser::yydefact_[] =
  {
       0,     0,     0,     2,     5,     0,     0,     0,     4,     1,
       3,     0,     0,     7,     0,     6,     9,     0,     8,    10,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    66,
      65,     0,     0,    35,    36,     0,     0,     0,     0,     0,
       0,     0,     0,    11,     0,     0,     0,    13,     0,     0,
      63,    62,     0,     0,    28,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      14,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    64,     0,
      40,    42,    39,    41,    37,    38,    59,    58,    52,    53,
      54,    55,    56,    57,     0,    34,    20,     0,    32,    18,
       0,     0,     0,     0,    12,     0,     0,    29,    15,     0,
      31,    17,     0,     0,    60,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    21,     0,     0,    33,    19,     0,
       0,     0,     0,     0,     0,     0,    51,    46,    45,    50,
      49,    44,    43,    48,    47,    27,    25,     0,    30,    16,
       0,    22,    24,    61,    26,     0,    23
  };

  const short
  fromParser::yypgoto_[] =
  {
     -36,   247,   -36,   -35,   -36,   -36,   -27
  };

  const signed char
  fromParser::yydefgoto_[] =
  {
       0,     2,     3,    32,    33,    34,    35
  };

  const unsigned char
  fromParser::yytable_[] =
  {
      50,    51,    13,    72,    53,    75,     1,    14,    27,    28,
      81,     9,    11,    84,    29,    30,    43,    31,    12,    44,
      45,     4,    46,    24,     5,    87,     6,    15,    89,    90,
      91,    92,    93,    94,    95,    96,    97,    98,    99,   100,
     101,   102,   103,   111,    16,     7,     8,    19,   125,    17,
      20,    21,    40,    22,    41,    62,    63,    64,    65,    66,
      67,    68,    69,    36,    47,    37,    25,    26,    48,    18,
      49,    23,    42,    64,    65,    66,    67,    68,    69,    55,
     140,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    38,   145,    39,   146,   147,
     148,   149,   150,   151,   152,   153,   154,   126,    73,   127,
      74,    52,    71,    62,    63,    64,    65,    66,    67,    68,
      69,   130,    54,   131,    76,    70,    77,    62,    63,    64,
      65,    66,    67,    68,    69,    62,    63,    64,    65,    66,
      67,    68,    69,    80,   128,   123,   129,   124,    69,    62,
      63,    64,    65,    66,    67,    68,    69,   132,    78,   133,
      79,   105,    62,    63,    64,    65,    66,    67,    68,    69,
      62,    63,    64,    65,    66,    67,    68,    69,    66,    67,
      68,    69,    88,    62,    63,    64,    65,    66,    67,    68,
      69,    82,   104,    83,   107,   163,    62,    63,    64,    65,
      66,    67,    68,    69,    63,    64,    65,    66,    67,    68,
      69,   114,   106,   108,    85,   115,    86,   116,   112,   141,
     113,   142,   109,   110,   117,   118,   119,   120,   121,   122,
     134,   135,   137,   136,   138,   143,   144,   155,   139,   156,
     158,   159,   157,   160,   161,   162,   164,   165,   166,     0,
      10
  };

  const short
  fromParser::yycheck_[] =
  {
      27,    28,     0,    38,    31,    40,    24,     5,    17,    18,
      45,     0,    24,    48,    23,    24,     0,    26,    24,     3,
       4,     0,     6,    24,     3,    52,     5,    25,    55,    56,
      57,    58,    59,    60,    61,    62,    63,    64,    65,    66,
      67,    68,    69,    78,     0,    24,    25,     0,     8,     5,
       3,     4,     4,     6,     6,    15,    16,    17,    18,    19,
      20,    21,    22,    24,     0,    26,    24,    24,     4,    25,
       6,    24,    24,    17,    18,    19,    20,    21,    22,     7,
     115,     9,    10,    11,    12,    13,    14,    15,    16,    17,
      18,    19,    20,    21,    22,     4,   123,     6,   125,   126,
     127,   128,   129,   130,   131,   132,   133,     9,    24,    11,
      26,    26,    24,    15,    16,    17,    18,    19,    20,    21,
      22,     9,     0,    11,    24,     0,    26,    15,    16,    17,
      18,    19,    20,    21,    22,    15,    16,    17,    18,    19,
      20,    21,    22,    24,    10,    25,    12,    27,    22,    15,
      16,    17,    18,    19,    20,    21,    22,    10,     4,    12,
       6,     0,    15,    16,    17,    18,    19,    20,    21,    22,
      15,    16,    17,    18,    19,    20,    21,    22,    19,    20,
      21,    22,    27,    15,    16,    17,    18,    19,    20,    21,
      22,    24,    27,    26,    24,    27,    15,    16,    17,    18,
      19,    20,    21,    22,    16,    17,    18,    19,    20,    21,
      22,     0,     0,     0,    24,     4,    26,     6,    24,    24,
      26,    26,     0,    24,     0,     0,    24,     0,     0,    24,
       0,    27,     0,    27,     0,    27,    27,     0,    24,     0,
       0,     0,    27,    24,     0,     0,     0,    27,     0,    -1,
       3
  };

  const signed char
  fromParser::yystos_[] =
  {
       0,    24,    29,    30,     0,     3,     5,    24,    25,     0,
      29,    24,    24,     0,     5,    25,     0,     5,    25,     0,
       3,     4,     6,    24,    24,    24,    24,    17,    18,    23,
      24,    26,    31,    32,    33,    34,    24,    26,     4,     6,
       4,     6,    24,     0,     3,     4,     6,     0,     4,     6,
      34,    34,    26,    34,     0,     7,     9,    10,    11,    12,
      13,    14,    15,    16,    17,    18,    19,    20,    21,    22,
       0,    24,    31,    24,    26,    31,    24,    26,     4,     6,
      24,    31,    24,    26,    31,    24,    26,    34,    27,    34,
      34,    34,    34,    34,    34,    34,    34,    34,    34,    34,
      34,    34,    34,    34,    27,     0,     0,    24,     0,     0,
      24,    31,    24,    26,     0,     4,     6,     0,     0,    24,
       0,     0,    24,    25,    27,     8,     9,    11,    10,    12,
       9,    11,    10,    12,     0,    27,    27,     0,     0,    24,
      31,    24,    26,    27,    27,    34,    34,    34,    34,    34,
      34,    34,    34,    34,    34,     0,     0,    27,     0,     0,
      24,     0,     0,    27,     0,    27,     0
  };

  const signed char
  fromParser::yyr1_[] =
  {
       0,    28,    29,    29,    30,    30,    30,    30,    30,    30,
      30,    30,    30,    30,    30,    30,    30,    30,    30,    30,
      30,    30,    30,    30,    30,    30,    30,    30,    30,    30,
      30,    30,    30,    30,    30,    31,    31,    32,    32,    32,
      32,    32,    32,    33,    33,    33,    33,    33,    33,    33,
      33,    33,    34,    34,    34,    34,    34,    34,    34,    34,
      34,    34,    34,    34,    34,    34,    34
  };

  const signed char
  fromParser::yyr2_[] =
  {
       0,     2,     1,     2,     2,     2,     3,     3,     4,     4,
       4,     6,     8,     6,     6,     8,    10,     8,     7,     8,
       7,     8,    10,    12,    10,     9,    10,     9,     6,     8,
      10,     8,     7,     8,     7,     1,     1,     3,     3,     3,
       3,     3,     3,     5,     5,     5,     5,     5,     5,     5,
       5,     5,     3,     3,     3,     3,     3,     3,     3,     3,
       4,     6,     2,     2,     3,     1,     1
  };


#if YYDEBUG || 1
  // YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
  // First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
  const char*
  const fromParser::yytname_[] =
  {
  "\"end of input\"", "error", "\"invalid token\"", "\"as\"", "\"on\"",
  "\"join\"", "\"using\"", "\"between\"", "\"and\"", "\"<=\"", "\">=\"",
  "\"<\"", "\">\"", "\"==\"", "\"!=\"", "\"|\"", "\"&\"", "\"+\"", "\"-\"",
  "\"*\"", "\"/\"", "\"%\"", "\"**\"", "\"numerical value\"", "\"name\"",
  "','", "'('", "')'", "$accept", "flist", "fterm", "compRange",
  "compRange2", "compRange3", "mathExpr", YY_NULLPTR
  };
#endif


#if YYDEBUG
  const short
  fromParser::yyrline_[] =
  {
       0,    88,    88,    88,    89,    93,    97,   101,   104,   108,
     112,   118,   124,   130,   136,   151,   166,   181,   196,   211,
     226,   241,   256,   271,   286,   301,   316,   331,   346,   353,
     360,   367,   374,   381,   388,   397,   397,   399,   409,   420,
     430,   440,   450,   463,   475,   487,   499,   511,   523,   535,
     547,   559,   574,   586,   598,   610,   622,   634,   646,   658,
     670,   682,   695,   705,   708,   711,   719
  };

  void
  fromParser::yy_stack_print_ () const
  {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator
           i = yystack_.begin (),
           i_end = yystack_.end ();
         i != i_end; ++i)
      *yycdebug_ << ' ' << int (i->state);
    *yycdebug_ << '\n';
  }

  void
  fromParser::yy_reduce_print_ (int yyrule) const
  {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1
               << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
      YY_SYMBOL_PRINT ("   $" << yyi + 1 << " =",
                       yystack_[(yynrhs) - (yyi + 1)]);
  }
#endif // YYDEBUG

  fromParser::symbol_kind_type
  fromParser::yytranslate_ (int t)
  {
    // YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to
    // TOKEN-NUM as returned by yylex.
    static
    const signed char
    translate_table[] =
    {
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      26,    27,     2,     2,    25,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24
    };
    // Last valid token kind.
    const int code_max = 279;

    if (t <= 0)
      return symbol_kind::S_YYEOF;
    else if (t <= code_max)
      return YY_CAST (symbol_kind_type, translate_table[t]);
    else
      return symbol_kind::S_YYUNDEF;
  }

#line 23 "fromParser.yy"
} // ibis
#line 2227 "fromParser.cc"

#line 728 "fromParser.yy"

void ibis::fromParser::error(const ibis::fromParser::location_type& l,
			     const std::string& m) {
    LOGGER(ibis::gVerbose >= 0)
	<< "Warning -- ibis::fromParser encountered " << m << " at location "
	<< l;
}
