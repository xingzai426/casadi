#
#     This file is part of CasADi.
# 
#     CasADi -- A symbolic framework for dynamic optimization.
#     Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
# 
#     CasADi is free software; you can redistribute it and/or
#     modify it under the terms of the GNU Lesser General Public
#     License as published by the Free Software Foundation; either
#     version 3 of the License, or (at your option) any later version.
# 
#     CasADi is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#     Lesser General Public License for more details.
# 
#     You should have received a copy of the GNU Lesser General Public
#     License along with CasADi; if not, write to the Free Software
#     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
# 
# 
from casadi import *
import numpy

# Let's construct a block diagonal structure
A = blkdiag([1,DMatrix([[2,3],[3,4]]),DMatrix([[5,6,7],[6,8,9],[7,9,10]]),11])
A.printMatrix()
A.sparsity().spy()

numpy.random.seed(2)

# We randomly permute this nice structure
perm =  numpy.random.permutation(range(A.size1()))
AP = A[perm,perm]

AP.printMatrix()
AP.sparsity().spy()

# And use stronglyConnectedComponents to recover the blocks
p = IVector()
r = IVector()
n = AP.sparsity(). stronglyConnectedComponents 	( p,r )

APrestored = AP[p,p]

APrestored.printMatrix()
APrestored.sparsity().spy()
print "# blocks: ", n
print "block boundaries: ", r[:n]
