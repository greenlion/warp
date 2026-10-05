// $Id$
//	Author: John Wu <John.Wu at ACM.org>
//              Lawrence Berkeley National Laboratory
//
// Copyright (c) 2000-2022, The Regents of the University of California,
// through Lawrence Berkeley National Laboratory (subject to receipt of any
// required approvals from the U.S. Dept. of Energy).  All rights reserved.
//
// If you have questions about your rights to use or distribute this
// software, please contact Berkeley Lab's Technology Transfer Department
// at TTD@lbl.gov.
//
// NOTICE.  This software is owned by the U.S. Department of Energy.  As
// such, the U.S. Government has been granted for itself and others acting
// on its behalf a paid-up, nonexclusive, irrevocable, worldwide license in
// the Software to reproduce, prepare derivative works, and perform
// publicly and display publicly.  Beginning five (5) years after the date
// permission to assert copyright is obtained from the U.S. Department of
// Energy, and subject to any subsequent five (5) year renewals, the
// U.S. Government is granted for itself and others acting on its behalf a
// paid-up, nonexclusive, irrevocable, worldwide license in the Software to
// reproduce, prepare derivative works, distribute copies to the public,
// perform publicly and display publicly, and to permit others to do so.

#ifndef IBIS_H
#define IBIS_H
/// @file ibis.h
///
/// The header file to be included by all user code.  It defines all
/// classes and functions intended to use  ibis::part interface.  All
/// such classes and functions are defined in the namespace  ibis.
/// Before performing any operations, the first function to be called is 
/// ibis::init.
///
/// @see ibis::init
#include "countQuery.h"		// ibis::countQuery
#include "meshQuery.h"		// ibis::meshQuery
#include "resource.h"		// ibis::gParameters
#include "bundle.h"		// ibis::bundle
#include "quaere.h"		// ibis::quaere
#include "query.h"		// ibis::query
#include "part.h"		// ibis::part, ibis::column, ibis::table
#include "blob.h"		// ibis::blob
#include "rids.h"		// ibis::ridHandler

/*! \mainpage Overview of FastBit IBIS Implementation

\author <A HREF="http://lbl.gov/~kwu/">John Wu</A>,
<A HREF="http://sdm.lbl.gov/">Scientific Data Management</A>,
<A HREF="http://www.lbl.gov/">Lawrence Berkeley National Lab</A>
\n With additional contributors listed in files
<A HREF="https://code.lbl.gov/plugins/scmsvn/viewcvs.php/trunk/AUTHORS?root=fastbit&view=markup">AUTHORS</A>
and
<A HREF="https://code.lbl.gov/plugins/scmsvn/viewcvs.php/trunk/ChangeLog?root=fastbit&view=markup">ChangeLog</A>.

\Copyright (c) 2000-2022
<A HREF="http://www.universityofcalifornia.edu/">University of California</A>

\section intro Introduction
FastBit is an open-source data processing library in the spirit of
<A HREF="http://en.wikipedia.org/wiki/NoSQL">NoSQL movement</A>.  It offers
a set of searching functions supported by compressed bitmap indexes.  It
recognizes user data in the column-oriented fashion similar to
<A HREF="http://monetdb.cwi.nl/">MonetDB</A> and
<A HREF="http://www.vertica.com/">Vertica</A>.  Because it is available as
a library, the users are free to build their own data processing system on
top of it.  In particular, the user data is NOT required to be under the
control of FastBit software.

The source code of FastBit is available at <A
HREF="https://code.lbl.gov/projects/fastbit/">http://goo.gl/JYMzu</A>
under <A HREF="http://crd.lbl.gov/~kewu/fastbit/src/license.txt">LGPL</A>.

\section bitmap Bitmap Index
An <A HREF="http://en.wikipedia.org/wiki/Index_%28database%29">index in a
database system</A> is a data structure that utilizes redundant information
about the base data to speed up common searching and retrieval operations.
Most of the commonly used indexes are variants of <A
HREF="http://portal.acm.org/citation.cfm?id=356776">B-trees</A>, such as
B+-tree and B*-tree.  FastBit implements a set of alternative indexes
called compressed <A HREF="http://en.wikipedia.org/wiki/Bitmap_index">bitmap
 indexes</A>.  Compared with B-tree variants, these indexes provide very
efficient searching and retrieval operations but are somewhat slower to
update after a modification of an individual record.

In addition to the well-known strengths of bitmap indexes, FastBit has a
special strength stemming from the bitmap compression scheme used.  The
compression method is called the <A
HREF="http://tinyurl.com/3chc2o">Word-Aligned Hybrid (WAH) code</A>.  It
reduces the bitmap indexes to reasonable sizes, and at the same time allows
very efficient bitwise logical operations directly on the compressed
bitmaps.  Compared with the well-known compression methods such as LZ77 and
<A HREF="http://tinyurl.com/2kwm5l">Byte-aligned Bitmap code</A> (BBC), WAH
sacrifices some space efficiency for a significant improvement in
operational efficiency [<A
HREF="http://crd.lbl.gov/%7Ekewu/ps/LBNL-49627.html">SSDBM 2002</A>, <A
HREF="http://crd.lbl.gov/%7Ekewu/ps/LBNL-48975.html">DOLAP 2001</A>].
Since the bitwise logical operations are the most important operations
needed to answer queries, using WAH compression has been shown to answer
queries significantly faster than using other compression schemes.

Theoretical analyses showed that WAH compressed bitmap indexes are <A
HREF="http://lbl.gov/%7Ekwu/ps/LBNL-49626.html">optimal for one-dimensional
range queries</A>.  Only the most efficient indexing schemes such as
B+-tree and B*-tree have this optimality property.  However, bitmap indexes
are superior because they can efficiently answer multi-dimensional range
queries by combining the answers to one-dimensional queries.

\section overview Key Components

FastBit process queries on one table at a time.  Currently, there are two
sets of interface for query processing, one more abstract and the other
more concrete.  The more abstract interface is represented by the class 
ibis::table and the more concrete interface is represented by the class 
ibis::part.  A table (with rows and columns) is divided into groups of rows
called data partitions.  Each data partition is stored in a column-wise
organization known as vertical projections.  At the abstract level,
queries on a table produces another table in the spirit of the relational
algebra.  At the concrete level, the queries on data partitions produce bit
vectors representing rows satisfying the user specified query conditions.

\subsection table Operations on Tables

The main class representing this interface is ibis::table.  The main query
function of this class is ibis::table::select, whose functionality
resembles a simplified form of the SELECT statement from the SQL language.
This function takes two string as arguments, one corresponds to the select
clause in SQL and the other corresponds to the where clause.  In the
following, we will call them the select clause and the where clause and
discuss the requirements and restriction on these clauses.  The function
ibis::table::select returns a new ibis::table when it completes
successfully.  This new table can be used in further query operations.

The select clause passed to function  ibis::table::select can only
contain column names separated by comma (,).  Aggregate operations such as
MIN, MAX, AVG, SUM, VARPOP, VARSAMP, STDPOP, STDSAMP, or DISTINCT are 
supported through another function named ibis::table::groupby.  
A group-by operation normally specified as one SQL statement needs to be 
split into two FastBit, one to select the values and
the other to perform the aggregation operations.  We've taken this approach
to simplify the implementation.  These aggregation operations are
not directly supported by bitmap indexes, therefore, they are not essential
to demonstrate the effectiveness of the bitmap indexes.

The where clause passed to function  ibis::table::select can be a
combination of range conditions connected with logical operators such as
AND, OR, XOR, and NOT.  Assuming that  temperature and  pressure are
names of two columns, the following are valid where clauses (one on each
line),

\code
temperature > 10000
pressure between 10 and 100
temperature > 10000 and 50 <= pressure and sin(pressure/8000) < sqrt(abs(temperature))
\endcode

The class  ibis::table also defines a set of functions for computing
histograms of various dimensions, namely,  ibis::table::getHistogram, 
ibis::table::getHistogram2D, and  ibis::table::getHistogram3D.

Using FastBit, one can only append new records to a table.  These
operations for extending a table is defined in the class  ibis::tablex.

For most fixed-sized data, such as integers and floating-point values,
FastBit functions expects raw binary data and also store them as raw
binary, therefore the data files and index files are not portable across
different platforms.  This is common to both  ibis::table interface and
 ibis::part interface.  However, one difference is that  ibis::table
handles string values as <code>std::vector<std::string></code>, while the
lower level interface  ibis::part handles strings as raw
<code>char*</code> with null terminators.

\subsection part Operations on Data Partitions

The two key classes for query processing on a data partition are 
ibis::part and  ibis::query, where the first represents the user data (or
base data) and the second represents a user query.  An  ibis::part is
primarily a container of  ibis::column objects and some common information
about the columns in a data partition.  The class  ibis::column has two
specialization for handling string values,  ibis::category for categorical
values (keys) and  ibis::text for arbitrary text strings.

The user query is represented as an  ibis::query object.  Each query is
associated with one  ibis::part object.  The functions of the query class
can be divided into three groups, (1) specifying a query, (2) evaluating a
query, and (3) retrieving information about the hits.  The queries accepted
by FastBit are a subset of the SQL SELECT statement.  Each query may have a
WHERE clause and optionally a SELECT clause.  Note that the FROM clause is
implicit in the association with an  ibis::part.  The WHERE clause is a
set of range conditions joined together with logical operators, e.g.,
<code>A = 5 AND (B between 6.5 and 8.2 OR C > sqrt(5*D))</code>.  The
SELECT clause can contain a list of column names and some of the four
functions AVG, MIN, MAX, SUM, VARPOP, VARSAMP, STDPOP, STDSAMP and DISTINCT.  
Each of the functions can only take a column name as its argument.  
If a SELECT clause is omitted, it is assumed to be "SELECT count(*)."  
We refer to this type of queries as count queries since their primary 
purpose is to count the number of hits.

To evaluate a query, one calls either ibis::query::estimate or
ibis::query::evaluate.  After a query is evaluated, one may call various
function to find the number of hits (ibis::query::getNumHits), the values
of selected rows (ibis::query::getQualifiedInts,
ibis::query::getQualifiedFloats, and ibis::query::getQualifiedDoubles), or
the bitvector that represents the hits (ibis::query::getHitVector).

\subsection indexes Indexes

The indexes are considered auxiliary data, therefore even though they
involve much more source files than  ibis::part and  ibis::query, they
are not essential from a users point of view.  In FastBit, the indexes are
usually built automatically as needed.  However, there are functions to
explicitly force FastBit to build them through  ibis::table::buildIndex,
 ibis::part::buildIndex and their variants.

Currently, all indexes are in a single class hierarchy with  ibis::index
as the abstract base class.  The most convenient way to create an index is
calling the class function  ibis::index::create.  One can control what
type of bitmap index to use by either specifying an index specification for
a whole table by calling  ibis::table::indexSpec, for a whole data
partition by calling  ibis::part::indexSpec, or for each individual
column by calling  ibis::column::indexSpec.  The index specification
along with other metadata are written to a file named
<code>-part.txt</code> in the directory containing the base data and the
index files.  The directory name is needed when constructing an ibis::part.
This information may be indirectly provided through an RC file specified to
the function  ibis::init.

\section ack Acknowledgments

The software programer gratefully acknowledges the support from the
research colleagues Kurt Stockinger, Ekow Otoo and Arie Shoshani.  They are
crucial in establishing the foundation of the FastBit system and applying
the software to a number of applications.  Many thanks to the early users.
Their generous feedback and suggestions are invaluable to the development
of the software.  A full list of contributors is in the file <A
HREF="https://code.lbl.gov/plugins/scmsvn/viewcvs.php/trunk/AUTHORS?root=fastbit&view=markup">AUTHORS</A>.
User feedback affecting the code and documentation is recorded in <A
HREF="https://code.lbl.gov/plugins/scmsvn/viewcvs.php/trunk/ChangeLog?root=fastbit&view=markup">ChangeLog</A>
along with user names.

This work was supported by the Director, Office of Science, Office of
Advanced Scientific Computing Research, of the U.S. Department of Energy
under Contract No. DE-AC02-05CH11231 and DE-AC03-76SF00098.  It also uses
resources of the <A HREF="http://nersc.gov/">National Energy Research
Scientific Computing Center</A>.

\section additional Additional Information

A general overview of FastBit work can be found at <http://su.pr/2vfyBE>.
More technical information is available on the web at
<http://sdm.lbl.gov/fastbit/> or <http://lbl.gov/~kwu/fastbit/>.

Send any comments, bug reports, and patches to
<fastbit-users@hpcrdm.lbl.gov>.

\section Copyright Notice
FastBit, Copyright (c) 2000-2022, The Regents of the University of California,
through Lawrence Berkeley National Laboratory (subject to receipt of any
required approvals from the U.S. Dept. of Energy).  All rights reserved.

If you have questions about your rights to use or distribute this software,
please contact Berkeley Lab's Technology Transfer Department at
TTD@lbl.gov.

NOTICE.  This software is owned by the U.S. Department of Energy.  As such,
the U.S. Government has been granted for itself and others acting on its
behalf a paid-up, nonexclusive, irrevocable, worldwide license in the
Software to reproduce, prepare derivative works, and perform publicly and
display publicly.  Beginning five (5) years after the date permission to
assert copyright is obtained from the U.S. Department of Energy, and
subject to any subsequent five (5) year renewals, the U.S. Government is
granted for itself and others acting on its behalf a paid-up, nonexclusive,
irrevocable, worldwide license in the Software to reproduce, prepare
derivative works, distribute copies to the public, perform publicly and
display publicly, and to permit others to do so.

\section License

"FastBit, Copyright (c) 2000-2022, The Regents of the University of
California, through Lawrence Berkeley National Laboratory (subject to
receipt of any required approvals from the U.S. Dept. of Energy).  All
rights reserved."

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

(1) Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

(2) Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

(3) Neither the name of the University of California, Lawrence Berkeley
National Laboratory, U.S. Dept. of Energy nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

You are under no obligation whatsoever to provide any bug fixes,
patches, or upgrades to the features, functionality or performance of
the source code ("Enhancements") to anyone; however, if you choose to
make your Enhancements available either publicly, or directly to
Lawrence Berkeley National Laboratory, without imposing a separate
written license agreement for such Enhancements, then you hereby grant
the following license: a non-exclusive, royalty-free perpetual license
to install, use, modify, prepare derivative works, incorporate into
other computer software, distribute, and sublicense such enhancements or
derivative works thereof, in binary and source code form.

*/


/// The current implementation of FastBit is code named IBIS; most data
/// structures and functions are in the name space ibis.  The name IBIS
/// could be considered as a short-hand for an Implementation of Bitmap
/// Indexing System or Ibis Bitmap Indexing System.
namespace ibis {
    // defined in src/iapi.cpp
    void init(const char* rcfile=0, const char* mesgfile=0);
}
#endif // IBIS_H
