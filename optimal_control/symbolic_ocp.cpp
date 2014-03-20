/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "symbolic_ocp.hpp"
#include "variable_internal.hpp"
#include "xml_node.hpp"

#include <map>
#include <string>
#include <sstream>
#include <ctime>

#include "symbolic/std_vector_tools.hpp"
#include "external_packages/tinyxml/tinyxml.h"

#include "../symbolic/casadi_exception.hpp"
#include "../symbolic/std_vector_tools.hpp"
#include "variable_tools.hpp"
#include "../symbolic/matrix/matrix_tools.hpp"
#include "../symbolic/sx/sx_tools.hpp"
#include "../symbolic/fx/integrator.hpp"
#include "../symbolic/casadi_calculus.hpp"

using namespace std;
namespace CasADi{

  // temporary
  vector<Variable> getVar(const SymbolicOCP& ocp, const SX& var){
    casadi_assert(var.isVector() && var.isSymbolic());
    vector<Variable> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      map<string,Variable>::const_iterator it = ocp.varmap_.find(var.at(i).getName());
      casadi_assert(it!=ocp.varmap_.end());
      ret[i] = it->second;
    }
    return ret;
  }

  SymbolicOCP::SymbolicOCP(){
    t = SX::sym("t");
    t0 = t0_guess = numeric_limits<double>::quiet_NaN();
    tf = tf_guess = numeric_limits<double>::quiet_NaN();
    t0_free = false;
    tf_free = false;

    // Start with vectors of zero length
    this->zQQQ = this->qQQQ = this->ciQQQ = this->cdQQQ = this->piQQQ = this->pdQQQ = this->pfQQQ = this->yQQQ = this->uQQQ = SX::zeros(0,1);
  }

  void SymbolicOCP::parseFMI(const std::string& filename){
    
    // Load 
    TiXmlDocument doc;
    bool flag = doc.LoadFile(filename.c_str());
    casadi_assert_message(flag, "Cound not open " << filename);

    // parse
    XMLNode document;
    document.addNode(&doc);

    // **** Add model variables ****
    {
      //if(verbose) cout << "Adding model variables." << endl;
  
      // Get a reference to the ModelVariables node
      const XMLNode& modvars = document[0]["ModelVariables"];

      // Add variables
      for(int i=0; i<modvars.size(); ++i){
      
        // Get a reference to the variable
        const XMLNode& vnode = modvars[i];

        // Get the attributes
        string name        = vnode.getAttribute("name");
        int valueReference;
        vnode.readAttribute("valueReference",valueReference);
        string variability = vnode.getAttribute("variability");
        string causality   = vnode.getAttribute("causality");
        string alias       = vnode.getAttribute("alias");
      
        // Skip to the next variable if its an alias
        if(alias.compare("alias") == 0 || alias.compare("negatedAlias") == 0)
          continue;
          
        // Get the name
        const XMLNode& nn = vnode["QualifiedName"];
        string qn = qualifiedName(nn);
      
        // Add variable, if not already added
        if(varmap_.find(qn)==varmap_.end()){
        
          // Create variable
          Variable var(name);
        
          // Value reference
          var.setValueReference(valueReference);
        
          // Variability
          if(variability.compare("constant")==0)
            var.setVariability(CONSTANT);
          else if(variability.compare("parameter")==0)
            var.setVariability(PARAMETER);
          else if(variability.compare("discrete")==0)
            var.setVariability(DISCRETE);
          else if(variability.compare("continuous")==0)
            var.setVariability(CONTINUOUS);
          else throw CasadiException("Unknown variability");
    
          // Causality
          if(causality.compare("input")==0)
            var.setCausality(INPUT);
          else if(causality.compare("output")==0)
            var.setCausality(OUTPUT);
          else if(causality.compare("internal")==0)
            var.setCausality(INTERNAL);
          else throw CasadiException("Unknown causality");
        
          // Alias
          if(alias.compare("noAlias")==0)
            var.setAlias(NO_ALIAS);
          else if(alias.compare("alias")==0)
            var.setAlias(ALIAS);
          else if(alias.compare("negatedAlias")==0)
            var.setAlias(NEGATED_ALIAS);
          else throw CasadiException("Unknown alias");
        
          // Other properties
          if(vnode.hasChild("Real")){
            const XMLNode& props = vnode["Real"];
            props.readAttribute("unit",var->unit_,false);
            props.readAttribute("displayUnit",var->displayUnit_,false);
            props.readAttribute("min",var->min_,false);
            props.readAttribute("max",var->max_,false);
            props.readAttribute("start",var->start_,false);
            props.readAttribute("nominal",var->nominal_,false);
            props.readAttribute("free",var->free_,false);
            props.readAttribute("initialGuess",var->initial_guess_,false);
          }
        
          // Variable category
          if(vnode.hasChild("VariableCategory")){
            string cat = vnode["VariableCategory"].getText();
            if(cat.compare("derivative")==0)
              var.setCategory(CAT_DERIVATIVE);
            else if(cat.compare("state")==0)
              var.setCategory(CAT_STATE);
            else if(cat.compare("dependentConstant")==0)
              var.setCategory(CAT_DEPENDENT_CONSTANT);
            else if(cat.compare("independentConstant")==0)
              var.setCategory(CAT_INDEPENDENT_CONSTANT);
            else if(cat.compare("dependentParameter")==0)
              var.setCategory(CAT_DEPENDENT_PARAMETER);
            else if(cat.compare("independentParameter")==0)
              var.setCategory(CAT_INDEPENDENT_PARAMETER);
            else if(cat.compare("algebraic")==0)
              var.setCategory(CAT_ALGEBRAIC);
            else throw CasadiException("Unknown variable category: " + cat);
          }
        
          // Add to list of variables
          addVariable(qn,var);
        }
      }
    }
  
    // **** Add binding equations ****
    {
      //if(verbose) cout << "Adding binding equations." << endl;
  
      // Get a reference to the BindingEquations node
      const XMLNode& bindeqs = document[0]["equ:BindingEquations"];
  
      for(int i=0; i<bindeqs.size(); ++i){
        const XMLNode& beq = bindeqs[i];

        // Get the variable
        Variable var = readVariable(beq[0]);

        // Get the binding equation
        SX bexpr = readExpr(beq[1][0]);
      
        // Add binding equation
        this->yQQQ.append(var->var_);
        this->dep.append(bexpr);
      }
    
      // Resort the dependant parameters
      sortDependentParameters();
    }

    // **** Add dynamic equations ****
    {
      // Get a reference to the DynamicEquations node
      const XMLNode& dyneqs = document[0]["equ:DynamicEquations"];

      // Add equations
      for(int i=0; i<dyneqs.size(); ++i){

        // Get a reference to the variable
        const XMLNode& dnode = dyneqs[i];

        // Add the differential equation
        SX de_new = readExpr(dnode[0]);
        dae.append(de_new);
      }
    }
  
    // **** Add initial equations ****
    {
      // Get a reference to the DynamicEquations node
      const XMLNode& initeqs = document[0]["equ:InitialEquations"];

      // Add equations
      for(int i=0; i<initeqs.size(); ++i){

        // Get a reference to the node
        const XMLNode& inode = initeqs[i];

        // Add the differential equations
        for(int i=0; i<inode.size(); ++i){
          initial.append(readExpr(inode[i]));
        }
      }
    }
  
    // **** Add optimization ****
    if(document[0].hasChild("opt:Optimization")){
    
      // Get a reference to the DynamicEquations node
      const XMLNode& opts = document[0]["opt:Optimization"];
    
      // Start time
      const XMLNode& intervalStartTime = opts["opt:IntervalStartTime"];
      if(intervalStartTime.hasChild("opt:Value"))
        intervalStartTime["opt:Value"].getText(t0);
      if(intervalStartTime.hasChild("opt:Free"))
        intervalStartTime["opt:Free"].getText(t0_free);
      if(intervalStartTime.hasChild("opt:InitialGuess"))
        intervalStartTime["opt:InitialGuess"].getText(t0_guess);

      // Terminal time
      const XMLNode& IntervalFinalTime = opts["opt:IntervalFinalTime"];
      if(IntervalFinalTime.hasChild("opt:Value"))
        IntervalFinalTime["opt:Value"].getText(tf);
      if(IntervalFinalTime.hasChild("opt:Free"))
        IntervalFinalTime["opt:Free"].getText(tf_free);
      if(IntervalFinalTime.hasChild("opt:InitialGuess"))
        IntervalFinalTime["opt:InitialGuess"].getText(tf_guess);

      // Time points
      const XMLNode& tpnode = opts["opt:TimePoints"];
      tp.resize(tpnode.size());
      for(int i=0; i<tp.size(); ++i){
        // Get index
        int index;
        tpnode[i].readAttribute("index",index);
      
        // Get value
        double value;
        tpnode[i].readAttribute("value",value);
        tp[i] = value;
      
        // Allocate all the timed variables
        for(int k=0; k<tpnode[i].size(); ++k){
          string qn = qualifiedName(tpnode[i][k]);
          atTime(qn,value,true);
        }
      }
    
      for(int i=0; i<opts.size(); ++i){
      
        // Get a reference to the node
        const XMLNode& onode = opts[i];

        // Get the type
        if(onode.checkName("opt:ObjectiveFunction")){ // mayer term
          try{
            // Add components
            for(int i=0; i<onode.size(); ++i){
              const XMLNode& var = onode[i];
            
              // If string literal, ignore
              if(var.checkName("exp:StringLiteral"))
                continue;
            
              // Read expression
              SX v = readExpr(var);
              mterm.append(v);
            }
          } catch(exception& ex){
            throw CasadiException(std::string("addObjectiveFunction failed: ") + ex.what());
          }
        } else if(onode.checkName("opt:IntegrandObjectiveFunction")){
          try{
            for(int i=0; i<onode.size(); ++i){
              const XMLNode& var = onode[i];

              // If string literal, ignore
              if(var.checkName("exp:StringLiteral"))
                continue;
            
              // Read expression
              SX v = readExpr(var);
              lterm.append(v);
            }
          } catch(exception& ex){
            throw CasadiException(std::string("addIntegrandObjectiveFunction failed: ") + ex.what());
          }
        } else if(onode.checkName("opt:IntervalStartTime")) {
          // Ignore, treated above
        } else if(onode.checkName("opt:IntervalFinalTime")) {
          // Ignore, treated above
        } else if(onode.checkName("opt:TimePoints")) {
          // Ignore, treated above
        } else if(onode.checkName("opt:PointConstraints")) {
          for(int i=0; i<onode.size(); ++i){
            const XMLNode& constr_i = onode[i];
            if(constr_i.checkName("opt:ConstraintLeq")){
              SX ex = readExpr(constr_i[0]);
              SX ub = readExpr(constr_i[1]);
              point.append(ex-ub);
              point_min.append(-numeric_limits<double>::infinity());
              point_max.append(0.);
            } else if(constr_i.checkName("opt:ConstraintGeq")){
              SX ex = readExpr(constr_i[0]);
              SX lb = readExpr(constr_i[1]);
              point.append(ex-lb);
              point_min.append(0.);
              point_max.append(numeric_limits<double>::infinity());
            } else if(constr_i.checkName("opt:ConstraintEq")){
              SX ex = readExpr(constr_i[0]);
              SX eq = readExpr(constr_i[1]);
              point.append(ex-eq);
              point_min.append(0.);
              point_max.append(0.);
            } else {
              cerr << "unknown constraint type" << constr_i.getName() << endl;
              throw CasadiException("SymbolicOCP::addConstraints");
            }
          }
        
        } else if(onode.checkName("opt:Constraints") || onode.checkName("opt:PathConstraints")) {
          for(int i=0; i<onode.size(); ++i){
            const XMLNode& constr_i = onode[i];
            if(constr_i.checkName("opt:ConstraintLeq")){
              SX ex = readExpr(constr_i[0]);
              SX ub = readExpr(constr_i[1]);
              path.append(ex-ub);
              path_min.append(-numeric_limits<double>::infinity());
              path_max.append(0.);
            } else if(constr_i.checkName("opt:ConstraintGeq")){
              SX ex = readExpr(constr_i[0]);
              SX lb = readExpr(constr_i[1]);
              path.append(ex-lb);
              path_min.append(0.);
              path_max.append(numeric_limits<double>::infinity());
            } else if(constr_i.checkName("opt:ConstraintEq")){
              SX ex = readExpr(constr_i[0]);
              SX eq = readExpr(constr_i[1]);
              path.append(ex-eq);
              path_min.append(0.);
              path_max.append(0.);
            } else {
              cerr << "unknown constraint type" << constr_i.getName() << endl;
              throw CasadiException("SymbolicOCP::addConstraints");
            }
          }
        
        } else throw CasadiException(string("SymbolicOCP::addOptimization: Unknown node ")+onode.getName());
      }
    }
  
    // Make sure that the dimensions are consistent at this point
    casadi_assert_warning(this->x.size()==this->ode.size(),"The number of differential equations (equations involving differentiated variables) does not match the number of differential states.");
    casadi_assert_warning(this->zQQQ.size()==this->alg.size(),"The number of algebraic equations (equations not involving differentiated variables) does not match the number of algebraic variables.");
    casadi_assert(this->qQQQ.size()==this->quad.size());
    casadi_assert(this->yQQQ.size()==this->dep.size());
  }

  Variable& SymbolicOCP::readVariable(const XMLNode& node){
    // Qualified name
    string qn = qualifiedName(node);
  
    // Find and return the variable
    return variable(qn);
  }

  SX SymbolicOCP::readExpr(const XMLNode& node){
    const string& fullname = node.getName();
    if (fullname.find("exp:")== string::npos) {
      casadi_error("SymbolicOCP::readExpr: unknown - expression is supposed to start with 'exp:' , got " << fullname);
    }
  
    // Chop the 'exp:'
    string name = fullname.substr(4);

    // The switch below is alphabetical, and can be thus made more efficient, for example by using a switch statement of the first three letters, if it would ever become a bottleneck
    if(name.compare("Add")==0){
      return readExpr(node[0]) + readExpr(node[1]);
    } else if(name.compare("Acos")==0){
      return acos(readExpr(node[0]));
    } else if(name.compare("Asin")==0){
      return asin(readExpr(node[0]));
    } else if(name.compare("Atan")==0){
      return atan(readExpr(node[0]));
    } else if(name.compare("Cos")==0){
      return cos(readExpr(node[0]));
    } else if(name.compare("Der")==0){
      Variable v = readVariable(node[0]);
      return v.der();
    } else if(name.compare("Div")==0){
      return readExpr(node[0]) / readExpr(node[1]);
    } else if(name.compare("Exp")==0){
      return exp(readExpr(node[0]));
    } else if(name.compare("Identifier")==0){
      return readVariable(node).var();
    } else if(name.compare("IntegerLiteral")==0){
      int val;
      node.getText(val);
      return val;
    } else if(name.compare("Instant")==0){
      double val;
      node.getText(val);
      return val;
    } else if(name.compare("Log")==0){
      return log(readExpr(node[0]));
    } else if(name.compare("LogLt")==0){ // Logical less than
      return readExpr(node[0]) < readExpr(node[1]);
    } else if(name.compare("LogGt")==0){ // Logical less than
      return readExpr(node[0]) > readExpr(node[1]);
    } else if(name.compare("Mul")==0){ // Multiplication
      return readExpr(node[0]) * readExpr(node[1]);
    } else if(name.compare("Neg")==0){
      return -readExpr(node[0]);
    } else if(name.compare("NoEvent")==0) {
      // NOTE: This is a workaround, we assume that whenever NoEvent occurs, what is meant is a switch
      int n = node.size();
    
      // Default-expression
      SX ex = readExpr(node[n-1]);
    
      // Evaluate ifs
      for(int i=n-3; i>=0; i -= 2) ex = if_else(readExpr(node[i]),readExpr(node[i+1]),ex);
    
      return ex;
    } else if(name.compare("Pow")==0){
      return pow(readExpr(node[0]),readExpr(node[1]));
    } else if(name.compare("RealLiteral")==0){
      double val;
      node.getText(val);
      return val;
    } else if(name.compare("Sin")==0){
      return sin(readExpr(node[0]));
    } else if(name.compare("Sqrt")==0){
      return sqrt(readExpr(node[0]));
    } else if(name.compare("StringLiteral")==0){
      throw CasadiException(node.getText());
    } else if(name.compare("Sub")==0){
      return readExpr(node[0]) - readExpr(node[1]);
    } else if(name.compare("Tan")==0){
      return tan(readExpr(node[0]));
    } else if(name.compare("Time")==0){
      return t.toScalar();
    } else if(name.compare("TimedVariable")==0){
      // Get the index of the time point
      int index;
      node.readAttribute("timePointIndex",index);
      return readVariable(node[0]).atTime(tp[index]);
    }

    // throw error if reached this point
    throw CasadiException(string("SymbolicOCP::readExpr: Unknown node: ") + name);
  
  }

  void SymbolicOCP::repr(std::ostream &stream) const{
    stream << "Flat OCP";
  }

  void SymbolicOCP::print(ostream &stream) const{
    stream << "Dimensions: "; 
    stream << "#s = " << this->s.size() << ", ";
    stream << "#x = " << this->x.size() << ", ";
    stream << "#z = " << this->zQQQ.size() << ", ";
    stream << "#q = " << this->qQQQ.size() << ", ";
    stream << "#y = " << this->yQQQ.size() << ", ";
    stream << "#pi = " << this->piQQQ.size() << ", ";
    stream << "#pd = " << this->pdQQQ.size() << ", ";
    stream << "#pf = " << this->pfQQQ.size() << ", ";
    stream << "#ci =  " << this->ciQQQ.size() << ", ";
    stream << "#cd =  " << this->cdQQQ.size() << ", ";
    stream << "#u = " << this->uQQQ.size() << ", ";
    stream << endl << endl;

    // Variables in the class hierarchy
    stream << "Variables" << endl;

    // Print the variables
    stream << "{" << endl;
    stream << "  t = " << this->t.getDescription() << endl;
    stream << "  s = " << this->s << endl;
    stream << "  x = " << this->x << endl;
    stream << "  z =  " << this->zQQQ << endl;
    stream << "  q =  " << this->qQQQ << endl;
    stream << "  y =  " << this->yQQQ << endl;
    stream << "  pi =  " << this->piQQQ << endl;
    stream << "  pd =  " << this->pdQQQ << endl;
    stream << "  pf =  " << this->pfQQQ << endl;
    stream << "  ci =  " << this->ciQQQ << endl;
    stream << "  cd =  " << this->cdQQQ << endl;
    stream << "  u =  " << this->uQQQ << endl;
    stream << "}" << endl;
  
    stream << "Fully-implicit differential-algebraic equations" << endl;
    for(int k=0; k<this->dae.size(); ++k){
      stream << "0 == " << this->dae.at(k) << endl;
    }
    stream << endl;

    stream << "Differential equations" << endl;
    for(int k=0; k<this->x.size(); ++k){
      stream << "0 == " << this->ode.at(k) << endl;
    }
    stream << endl;

    stream << "Algebraic equations" << endl;
    for(int k=0; k<this->zQQQ.size(); ++k){
      stream << "0 == " << this->alg.at(k) << endl;
    }
    stream << endl;
  
    stream << "Quadrature equations" << endl;
    for(int k=0; k<this->qQQQ.size(); ++k){
      stream << der(this->qQQQ.at(k)).toScalar() << " == " << this->quad.at(k) << endl;
    }
    stream << endl;

    stream << "Initial equations" << endl;
    for(SX::const_iterator it=this->initial.begin(); it!=this->initial.end(); it++){
      stream << "0 == " << *it << endl;
    }
    stream << endl;

    // Dependent equations
    stream << "Dependent equations" << endl;
    for(int i=0; i<this->yQQQ.size(); ++i)
      stream << this->yQQQ.at(i) << " == " << this->dep.at(i) << endl;
    stream << endl;

    // Mayer terms
    stream << "Mayer objective terms" << endl;
    for(int i=0; i<this->mterm.size(); ++i)
      stream << this->mterm.at(i) << endl;
    stream << endl;
  
    // Lagrange terms
    stream << "Lagrange objective terms" << endl;
    for(int i=0; i<this->lterm.size(); ++i)
      stream << this->lterm.at(i) << endl;
    stream << endl;
  
    // Path constraint functions
    stream << "Path constraint functions" << endl;
    for(int i=0; i<this->path.size(); ++i)
      stream << this->path_min.at(i) << " <= " << this->path.at(i) << " <= " << this->path_max.at(i) << endl;
    stream << endl;
  
    // Point constraint functions
    stream << "Point constraint functions" << endl;
    for(int i=0; i<this->point.size(); ++i)
      stream << this->point_min.at(i) << " <= " << this->point.at(i) << " <= " << this->point_max.at(i) << endl;
    stream << endl;
  
    // Constraint functions
    stream << "Time horizon" << endl;
    stream << "t0 = " << this->t0 << endl;
    stream << "tf = " << this->tf << endl;
    stream << "tp = " << this->tp << endl;
  }

  void SymbolicOCP::eliminateInterdependencies(){
    substituteInPlace(this->yQQQ,this->dep,false);
  
    // Make sure that the dependent variables have been properly eliminated from the dependent expressions
    casadi_assert(!dependsOn(this->dep,this->yQQQ));
  }

  vector<SX> SymbolicOCP::substituteDependents(const vector<SX>& x) const{
    return substitute(x,vector<SX>(1,this->yQQQ),vector<SX>(1,this->dep));
  }

  void SymbolicOCP::eliminateDependent(bool eliminate_dependents_with_bounds){
    // All the functions to be replaced
    vector<SX> fcn(7);
    fcn[0] = this->ode;
    fcn[1] = this->alg;
    fcn[2] = this->quad;
    fcn[3] = this->initial;
    fcn[4] = this->path;
    fcn[5] = this->mterm;
    fcn[6] = this->lterm;
  
    // Replace all at once
    vector<SX> fcn_new = substituteDependents(fcn);
  
    // Save the new expressions
    this->ode = fcn_new[0];
    this->alg = fcn_new[1];
    this->quad = fcn_new[2];
    this->initial = fcn_new[3];
    this->path    = fcn_new[4];
    this->mterm   = fcn_new[5];
    this->lterm   = fcn_new[6];
  }

  void SymbolicOCP::eliminateLagrangeTerms(){
    // Index for the names
    int ind = 0;
    // For every integral term in the objective function
    for(SX::iterator it=this->lterm.begin(); it!=this->lterm.end(); ++it){
    
      // Give a name to the quadrature state
      stringstream q_name;
      q_name << "q_" << ind++;
    
      // Create a new quadrature state
      Variable qv(q_name.str());
      qv.setVariability(CONTINUOUS);
      qv.setCausality(INTERNAL);
      qv.setStart(0.0);
      if(tf==tf) qv.setNominal(this->tf); // if not not-a-number
  
      // Add to the list of variables
      addVariable(q_name.str(),qv);
    
      // Add to the quadrature states
      this->qQQQ.append(qv.var());

      // Add the Lagrange term to the list of quadratures
      this->quad.append(*it);
    
      // Add to the list of Mayer terms
      this->mterm.append(qv.var());
    }
  
    // Remove the Lagrange terms
    this->lterm.clear();
  }

  void SymbolicOCP::eliminateQuadratureStates(){
  
    // Move all the quadratures to the list of differential states
    vector<Variable> yy = getVar(*this,this->qQQQ);
    this->x.insert(this->x.end(),yy.begin(),yy.end());
    this->qQQQ = SX::zeros(0,1);
  
    // Move the equations to the list of ODEs
    this->ode.append(this->quad);
    this->quad = SX::zeros(0,1);
  }

  void SymbolicOCP::scaleVariables(){
    cout << "Scaling variables ..." << endl;
    double time1 = clock();
  
    // Variables
    SX _s = CasADi::var(this->s);
    SX _sdot = der(var(this->s));
    SX _x = CasADi::var(this->x);
  
    // Collect all the variables
    SX v;
    v.append(this->t);
    v.append(_s);
    v.append(_sdot);
    v.append(_x);
    v.append(this->zQQQ);
    v.append(this->piQQQ);
    v.append(this->pfQQQ);
    v.append(this->uQQQ);
    
    // Nominal values
    SX t_n = 1.;
    SX s_n = nominal(_s);
    SX x_n = nominal(_x);
    SX z_n = nominal(this->zQQQ);
    SX pi_n = nominal(this->piQQQ);
    SX pf_n = nominal(this->pfQQQ);
    SX u_n = nominal(this->uQQQ);
  
    // Get all the old variables in expressed in the nominal ones
    SX v_old;
    v_old.append(this->t*t_n);
    v_old.append(_s*s_n);
    v_old.append(_sdot*s_n);
    v_old.append(_x*x_n);
    v_old.append(this->zQQQ*z_n);
    v_old.append(this->piQQQ*pi_n);
    v_old.append(this->pfQQQ*pf_n);
    v_old.append(this->uQQQ*u_n);
  
    // Temporary variable
    SX temp;

    // Substitute equations
    this->dae = substitute(this->dae,v,v_old);
    this->ode = substitute(this->ode,v,v_old);
    this->alg = substitute(this->alg,v,v_old);
    this->quad = substitute(this->quad,v,v_old);
    this->dep = substitute(this->dep,v,v_old);
    this->initial = substitute(this->initial,v,v_old);
    this->path    = substitute(this->path,v,v_old);
    this->mterm   = substitute(this->mterm,v,v_old);
    this->lterm   = substitute(this->lterm,v,v_old);
  
    double time2 = clock();
    double dt = double(time2-time1)/CLOCKS_PER_SEC;
    cout << "... variable scaling complete after " << dt << " seconds." << endl;
  }
    
  void SymbolicOCP::scaleEquations(){
  
    cout << "Scaling equations ..." << endl;
    double time1 = clock();

    // Variables
    enum Variables{T,X,XDOT,Z,PI,PF,U,NUM_VAR};
    vector<SX > v(NUM_VAR); // all variables
    v[T] = this->t;
    v[X] = var(this->x);
    v[XDOT] = der(var(this->x));
    v[Z] = this->zQQQ;
    v[PI] = this->piQQQ;
    v[PF] = this->pfQQQ;
    v[U] = this->uQQQ;

    // Create the jacobian of the implicit equations with respect to [x,z,p,u] 
    SX xz;
    xz.append(v[X]);
    xz.append(v[Z]);
    xz.append(v[PI]);
    xz.append(v[PF]);
    xz.append(v[U]);
    SXFunction fcn = SXFunction(xz,ode);
    SXFunction J(v,fcn.jac());

    // Evaluate the Jacobian in the starting point
    J.init();
    J.setInput(0.0,T);
    J.setInput(start(var(this->x),true),X);
    J.input(XDOT).setAll(0.0);
    J.setInput(start(this->zQQQ,true),Z);
    J.setInput(start(this->piQQQ,true),PI);
    J.setInput(start(this->pfQQQ,true),PF);
    J.setInput(start(this->uQQQ,true),U);
    J.evaluate();
  
    // Get the maximum of every row
    Matrix<double> &J0 = J.output();
    vector<double> scale(J0.size1(),0.0); // scaling factors
    for(int cc=0; cc<J0.size2(); ++cc){
      // Loop over non-zero entries of the column
      for(int el=J0.colind(cc); el<J0.colind(cc+1); ++el){
        // Row
        int rr=J0.row(el);
      
        // The scaling factor is the maximum norm, ignoring not-a-number entries
        if(!isnan(J0.at(el))){
          scale[rr] = std::max(scale[rr],fabs(J0.at(el)));
        }
      }
    }
  
    // Make sure nonzero factor found
    for(int rr=0; rr<J0.size1(); ++rr){
      if(scale[rr]==0){
        cout << "Warning: Could not generate a scaling factor for equation " << rr << "(0 == " << ode.at(rr) << "), selecting 1." << endl;
        scale[rr]=1.;
      }
    }
  
    // Scale the equations
    for(int i=0; i<ode.size(); ++i){
      ode[i] /= scale[i];
    }
  
    double time2 = clock();
    double dt = double(time2-time1)/CLOCKS_PER_SEC;
    cout << "... equation scaling complete after " << dt << " seconds." << endl;
  }

  void SymbolicOCP::sortDAE(){
    // Quick return if no differential states
    if(this->x.empty()) return;

    // Find out which differential equation depends on which differential state
    SXFunction f(der(var(this->s)),this->dae);
    f.init();
    Sparsity sp = f.jacSparsity();
  
    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.dulmageMendelsohn(rowperm,colperm,rowblock,colblock,coarse_rowblock,coarse_colblock);

    // Permute equations
    this->dae = this->dae(rowperm);
  
    // Permute variables
    vector<Variable> s_new(this->s.size());
    for(int i=0; i<this->s.size(); ++i){
      s_new[i]= this->s[colperm[i]];
    }
    s_new.swap(this->s);
  }

  void SymbolicOCP::sortALG(){
    // Quick return if no algebraic states
    if(this->zQQQ.isEmpty()) return;
  
    // Find out which algebraic equation depends on which algebraic state
    SXFunction f(this->zQQQ,this->alg);
    f.init();
    Sparsity sp = f.jacSparsity();
  
    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.dulmageMendelsohn(rowperm,colperm,rowblock,colblock,coarse_rowblock,coarse_colblock);

    // Permute equations
    this->alg = this->alg(rowperm);
  
    // Permute variables
    this->zQQQ = this->zQQQ(colperm);
  }

  void SymbolicOCP::sortDependentParameters(){
    // Quick return if no dependent parameters
    if(this->pdQQQ.isEmpty()) return;
  
    // Find out which dependent parameter depends on which binding equation
    SX v = this->pdQQQ;
    SXFunction f(v,v-substitute(this->pdQQQ,this->yQQQ,dep));
    f.init();
    Sparsity sp = f.jacSparsity();
  
    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    sp.dulmageMendelsohn(rowperm,colperm,rowblock,colblock,coarse_rowblock,coarse_colblock);

    // Permute variables
    this->pdQQQ = this->pdQQQ(colperm);
  }

  void SymbolicOCP::makeExplicit(){
    // Quick return if there are no implicitly defined states
    if(this->s.empty()) return;
    
    // Write the ODE as a function of the state derivatives
    SXFunction f(der(var(this->s)),this->dae);
    f.init();

    // Get the sparsity of the Jacobian which can be used to determine which variable can be calculated from which other
    Sparsity sp = f.jacSparsity();

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    int nb = sp.dulmageMendelsohn(rowperm,colperm,rowblock,colblock,coarse_rowblock,coarse_colblock);

    // Permute equations
    this->dae = this->dae(rowperm);
  
    // Permute variables
    vector<Variable> s_new(this->s.size());
    for(int i=0; i<this->s.size(); ++i){
      s_new[i]= this->s[colperm[i]];
    }
    s_new.swap(this->s);
    s_new.clear();

    // Now write the sorted ODE as a function of the state derivatives
    f = SXFunction(der(var(this->s)),this->dae);
    f.init();

    // Get the Jacobian
    SX J = f.jac();
  
    // Block variables and equations
    vector<Variable> xb, xdb, xab;

    // Explicit ODE
    SX new_ode;
  
    // Loop over blocks
    for(int b=0; b<nb; ++b){
    
      // Block size
      int bs = rowblock[b+1] - rowblock[b];
    
      // Get variables in the block
      xb.clear();
      for(int i=colblock[b]; i<colblock[b+1]; ++i){
        xb.push_back(this->s[i]);
      }

      // Get equations in the block
      SX fb = dae(Slice(rowblock[b],rowblock[b+1]));

      // Get local Jacobian
      SX Jb = J(Slice(rowblock[b],rowblock[b+1]),Slice(colblock[b],colblock[b+1]));

      // If Jb depends on xb, then the state derivative does not enter linearly in the ODE and we cannot solve for the state derivative
      casadi_assert_message(!dependsOn(Jb,der(var(xb))),"Cannot find an explicit expression for variable(s) " << xb);
      
      // Divide fb into a part which depends on vb and a part which doesn't according to "fb == mul(Jb,vb) + fb_res"
      SX fb_res = substitute(fb,der(var(xb)),SX::zeros(xb.size())).data();
      SX fb_exp;
      
      // Solve for vb
      if (bs <= 3){
        // Calculate inverse and multiply for very small matrices
        fb_exp = mul(inv(Jb),-fb_res);
      } else {
        // QR factorization
        fb_exp = solve(Jb,-fb_res);
      }

      // Add to explicitly determined equations and variables
      new_ode.append(fb_exp);
    }
  
    // Eliminate inter-dependencies
    substituteInPlace(der(var(this->s)),new_ode,false);

    // Add to explicit differential states and ODE
    this->ode.append(new_ode);
    this->x.insert(this->x.end(),this->s.begin(),this->s.end());
    this->s.clear();
  }

  void SymbolicOCP::eliminateAlgebraic(){
    // Quick return if there are no algebraic states
    if(this->zQQQ.isEmpty()) return;
  
    // Write the algebraic equations as a function of the algebraic states
    SXFunction f(this->zQQQ,alg);
    f.init();

    // Get the sparsity of the Jacobian which can be used to determine which variable can be calculated from which other
    Sparsity sp = f.jacSparsity();

    // BLT transformation
    vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    int nb = sp.dulmageMendelsohn(rowperm,colperm,rowblock,colblock,coarse_rowblock,coarse_colblock);

    // Permute equations
    this->alg = this->alg(rowperm);
  
    // Permute variables
    this->zQQQ = this->zQQQ(colperm);

    // Rewrite the sorted algebraic equations as a function of the algebraic states
    f = SXFunction(this->zQQQ,this->alg);
    f.init();

    // Get the Jacobian
    SX J = f.jac();
  
    // Variables where we have found an explicit expression and where we haven't
    SX z_exp, z_imp;
  
    // Explicit and implicit equations
    SX f_exp, f_imp;
  
    // Loop over blocks
    for(int b=0; b<nb; ++b){
    
      // Block size
      int bs = rowblock[b+1] - rowblock[b];
    
      // Get local variables
      SX zb = this->zQQQ(Slice(colblock[b],colblock[b+1]));

      // Get local equations
      SX fb = this->alg(Slice(rowblock[b],rowblock[b+1]));

      // Get local Jacobian
      SX Jb = J(Slice(rowblock[b],rowblock[b+1]),Slice(colblock[b],colblock[b+1]));

      // If Jb depends on zb, then we cannot (currently) solve for it explicitly
      if(dependsOn(Jb,zb)){
      
        // Add the equations to the new list of algebraic equations
        f_imp.append(fb);
        
        // ... and the variables accordingly
        z_imp.append(zb);
      
      } else { // The variables that we wish to determine enter linearly
      
        // Divide fb into a part which depends on vb and a part which doesn't according to "fb == mul(Jb,vb) + fb_res"
        SX fb_res = substitute(fb,zb,SX::zeros(zb.sparsity()));
      
        // Solve for vb
        SX fb_exp;
        if (bs <= 3){
          // Calculate inverse and multiply for very small matrices
          fb_exp = mul(inv(Jb),-fb_res);
        } else {
          // QR factorization
          fb_exp = solve(Jb,-fb_res);
        }

        // Add to explicitly determined equations and variables
        z_exp.append(zb);
        f_exp.append(fb_exp);
      }
    }
  
    // Eliminate inter-dependencies in fb_exp
    substituteInPlace(z_exp,f_exp,false);

    // Add to the beginning of the dependent variables (since the other dependent variable might depend on them)
    this->yQQQ = vertcat(z_exp,this->yQQQ);
    this->dep = vertcat(f_exp,this->dep);
  
    // Save new algebraic equations
    this->zQQQ = z_imp;
    this->alg = f_imp;
  
    // Eliminate new dependent variables from the other equations
    eliminateDependent();
  }

  void SymbolicOCP::makeAlgebraic(const std::string& name){
    makeAlgebraic(variable(name));
  }

  void SymbolicOCP::makeAlgebraic(const Variable& v){
    casadi_assert(0);
#if 0
    // Find variable among the explicit variables
    for(int k=0; k<this->x.size(); ++k){
      if(this->x[k].get()==v.get()){
      
        // Add to list of algebraic variables and to the list of algebraic equations
        this->zQQQ.append(v->var_);
        this->alg.append(this->ode.at(k));
      
        // Remove from list of differential variables and the list of differential equations
        this->x.erase(this->x.begin()+k);
        vector<SXElement> ode_ = this->ode.data();
        ode_.erase(ode_.begin()+k);
        this->ode = ode_;

        // Successfull return
        return;
      }
    }
  
    // Find the variable among the implicit variables
    for(int k=0; k<xz.size(); ++k){
      if(xz[k].get()==v.get()){
      
        // Substitute the state derivative with zero
        this->dae = substitute(this->dae,xz[k].der(),0.0);

        // Remove the highest state derivative expression from the variable
        xz[k].setDifferential(false);

        // Successfull return
        return;
      }
    }
  
    // Error if this point reached
    throw CasadiException("v not a differential state");
#endif
  }

  Variable& SymbolicOCP::variable(const std::string& name){
    // Find the variable
    map<string,Variable>::iterator it = varmap_.find(name);
    if(it==varmap_.end()){
      casadi_error("No such variable: \"" << name << "\".");
    }
  
    // Return the variable
    return it->second;
  }

  void SymbolicOCP::addVariable(const std::string& name, const Variable& var){
    // Try to find the component
    map<string,Variable>::iterator it = varmap_.find(name);
    if(it!=varmap_.end()){
      stringstream ss;
      ss << "Variable \"" << name << "\" has already been added.";
      throw CasadiException(ss.str());
    }
  
    // Add to the map of all variables
    varmap_[name] = var;
  
    // Sort by category
    switch(var.getCategory()){
    case CAT_DERIVATIVE:
      // Skip derivatives
      break;
    case CAT_STATE:
      this->s.push_back(var);
      break;
    case CAT_DEPENDENT_CONSTANT:
      this->cdQQQ.append(var->var_);
      break;
    case CAT_INDEPENDENT_CONSTANT:
      this->ciQQQ.append(var->var_);
      break;
    case CAT_DEPENDENT_PARAMETER:
      this->pdQQQ.append(var->var_);
      break;
    case CAT_INDEPENDENT_PARAMETER:
      if(var.getFree()){
        this->pfQQQ.append(var->var_);
      } else {
        this->piQQQ.append(var->var_);
      }
      break;
    case CAT_ALGEBRAIC:
      if(var.getCausality() == INTERNAL){
        this->s.push_back(var);
      } else if(var.getCausality() == INPUT){
        this->uQQQ.append(var->var_);
      }
      break;
    default:
      casadi_assert_message(0,"Unknown category");
    }
  }

  std::string SymbolicOCP::qualifiedName(const XMLNode& nn){
    // Stringstream to assemble name
    stringstream qn;
  
    for(int i=0; i<nn.size(); ++i){
      // Add a dot
      if(i!=0) qn << ".";
    
      // Get the name part
      qn << nn[i].getAttribute("name");

      // Get the index, if any
      if(nn[i].size()>0){
        int ind;
        nn[i]["exp:ArraySubscripts"]["exp:IndexExpression"]["exp:IntegerLiteral"].getText(ind);
        qn << "[" << ind << "]";
      }
    }
  
    // Return the name
    return qn.str();
  }

  void SymbolicOCP::generateMuscodDatFile(const std::string& filename, const Dictionary& mc2_ops) const{
    // Print
    cout << "Generating: " << filename << endl;
  
    // Create the datfile
    ofstream datfile;
    datfile.open(filename.c_str());
    datfile.precision(numeric_limits<double>::digits10+2);
    datfile << scientific; // This is really only to force a decimal dot, would be better if it can be avoided
  
    // Print header
    datfile << "* This function was automatically generated by CasADi" << endl;
    datfile << endl;

    // User set options
    for(Dictionary::const_iterator it=mc2_ops.begin(); it!=mc2_ops.end(); ++it){
      // Print the name
      datfile << it->first << endl;
    
      // Get the value
      const GenericType& val = it->second;
    
      // Print value
      if(val.isInt()){
        datfile << int(val) << endl;
      } else if(val.isDouble()){
        datfile << double(val) << endl;
      } else if(val.isString()){
        datfile << string(val) << endl;
      } else if(val.isIntVector()){
        vector<int> valv = val;
        for(int k=0; k<valv.size(); ++k){
          datfile << k << ": " << valv[k] << endl;
        }
      } else if(val.isDoubleVector()){
        vector<double> valv = val;
        for(int k=0; k<valv.size(); ++k){
          datfile << k << ": " << valv[k] << endl;
        }
      } else if(val.isStringVector()){
        vector<string> valv = val;
        for(int k=0; k<valv.size(); ++k){
          datfile << k << ": " << valv[k] << endl;
        }
      }
      datfile << endl;
    }
  
    // Get the stage duration
    double h = tf-t0;

    // Is the stage duration fixed?
    bool h_fix = !t0_free && !tf_free;
  
    // Get bounds on the stage duration
    double h_min=h, h_max=h;
  
    // Set to some dummy variables if stage duration not fixed
    if(!h_fix){
      casadi_warning("h_min and h_max being set to dummy variables!");
      h_min = 0;
      h_max = numeric_limits<double>::infinity();
    }
  
    datfile << "* model stage duration start values, scale factors, and bounds" << endl;
    datfile << "h" << endl;
    datfile << "0: " << h << endl;
    datfile << endl;

    datfile << "h_sca" << endl;
    datfile << "0: " << h << endl;
    datfile << endl;
  
    datfile << "h_min" << endl;
    datfile << "0: " << h_min << endl;
    datfile << endl;
  
    datfile << "h_max" << endl;
    datfile << "0: " << h_max << endl;
    datfile << endl;
  
    datfile << "h_fix" << endl;
    datfile << "0: " << h_fix << endl;
    datfile << endl;
    
    // Parameter properties
    SX p = vertcat(this->piQQQ,this->pfQQQ);
    if(!p.isEmpty()){
      datfile << "*  global model parameter start values, scale factors, and bounds" << endl;
      datfile << "p" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << start(p[k]) << endl;
      }
      datfile << endl;
    
      datfile << "p_sca" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << nominal(p[k]) << endl;
      }
      datfile << endl;
    
      datfile << "p_min" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << min(p[k]) << endl;
      }
      datfile << endl;
    
      datfile << "p_max" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << max(p[k]) << endl;
      }
      datfile << endl;
    
      datfile << "p_fix" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << (min(p[k])==max(p[k])) << endl;
      }
      datfile << endl;
    
      datfile << "p_name" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << p[k].getName() << endl;
      }
      datfile << endl;
    
      datfile << "p_unit" << endl;
      for(int k=0; k<p.size(); ++k){
        datfile << k << ": " << unit(p[k]) << endl;
      }
      datfile << endl;
    }

    // Differential state properties
    if(!x.empty()){
      datfile << "*  differential state start values, scale factors, and bounds" << endl;
      datfile << "sd(*,*)" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << x[k].getStart() << endl;
      }
      datfile << endl;
    
      datfile << "sd_sca(*,*)" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << x[k].getNominal() << endl;
      }
      datfile << endl;
    
      datfile << "sd_min(*,*)" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << x[k].getMin() << endl;
      }
      datfile << endl;
    
      datfile << "sd_max(*,*)" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << x[k].getMax() << endl;
      }
      datfile << endl;
    
      datfile << "sd_fix(*,*)" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << (x[k].getMin()==x[k].getMax()) << endl;
      }
      datfile << endl;

      datfile << "xd_name" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << x[k].getName() << endl;
      }
      datfile << endl;

      datfile << "xd_unit" << endl;
      for(int k=0; k<x.size(); ++k){
        datfile << k << ": " << x[k].getUnit() << endl;
      }
      datfile << endl;
    }
  
    // Algebraic state properties
    if(!this->zQQQ.isEmpty()){
      datfile << "*  algebraic state start values, scale factors, and bounds" << endl;
      datfile << "sa(*,*)" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << start(this->zQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "sa_sca(*,*)" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << nominal(this->zQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "sa_min(*,*)" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << min(this->zQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "sa_max(*,*)" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << max(this->zQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "sa_fix(*,*)" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << (min(this->zQQQ[k])==max(this->zQQQ[k])) << endl;
      }
      datfile << endl;

      datfile << "xa_name" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << this->zQQQ[k].getName() << endl;
      }
      datfile << endl;
    
      datfile << "xa_unit" << endl;
      for(int k=0; k<this->zQQQ.size(); ++k){
        datfile << k << ": " << unit(this->zQQQ[k]) << endl;
      }
      datfile << endl;
    }
  
    // Control properties
    if(!this->uQQQ.isEmpty()){
      datfile << "* control start values, scale factors, and bounds" << endl;
      datfile << "u(*,*)" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << start(this->uQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "u_sca(*,*)" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << nominal(this->uQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "u_min(*,*)" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << min(this->uQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "u_max(*,*)" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << max(this->uQQQ[k]) << endl;
      }
      datfile << endl;
    
      datfile << "u_fix(*,*)" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << (min(this->uQQQ[k])==max(this->uQQQ[k])) << endl;
      }
      datfile << endl;
    
      datfile << "u_name" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << this->uQQQ[k].getName() << endl;
      }
      datfile << endl;
    
      datfile << "u_unit" << endl;
      for(int k=0; k<this->uQQQ.size(); ++k){
        datfile << k << ": " << unit(this->uQQQ[k]) << endl;
      }
      datfile << endl;
    }

    // Close the datfile
    datfile.close();
  }

  SX SymbolicOCP::operator()(const std::string& name) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return it->second.var();
  }

  SX SymbolicOCP::der(const std::string& name) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return it->second.der();
  } 

  SX SymbolicOCP::der(const SX& var) const{
    casadi_assert(var.isVector() && var.isSymbolic());
    SX ret = SX::zeros(var.sparsity());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = der(var.at(i).getName());
    }
    return ret;
  }

  double SymbolicOCP::nominal(const std::string& name) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return it->second.getNominal();
  }

  void SymbolicOCP::setNominal(const std::string& name, double val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second.setNominal(val);
  }

  double SymbolicOCP::min(const std::string& name, bool nominal) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return nominal ? it->second.getMin() / it->second.getNominal() : it->second.getMin();
  }

  void SymbolicOCP::setMin(const std::string& name, double val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second.setMin(val);
  }

  double SymbolicOCP::max(const std::string& name, bool nominal) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return nominal ? it->second.getMax() / it->second.getNominal() : it->second.getMax();
  }

  void SymbolicOCP::setMax(const std::string& name, double val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second.setMax(val);
  }

  double SymbolicOCP::start(const std::string& name, bool nominal) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return nominal ? it->second.getStart() / it->second.getNominal() : it->second.getStart();
  }

  void SymbolicOCP::setStart(const std::string& name, double val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second.setStart(val);
  }

  double SymbolicOCP::initialGuess(const std::string& name, bool nominal) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return nominal ? it->second.getInitialGuess() / it->second.getNominal() : it->second.getInitialGuess();
  }

  void SymbolicOCP::setInitialGuess(const std::string& name, double val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second.setInitialGuess(val);
  }

  double SymbolicOCP::derivativeStart(const std::string& name, bool nominal) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return nominal ? it->second.getDerivativeStart() / it->second.getNominal() : it->second.getDerivativeStart();
  }

  void SymbolicOCP::setDerivativeStart(const std::string& name, double val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second.setDerivativeStart(val);
  }

  SX SymbolicOCP::atTime(const std::string& name, double t, bool allocate) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return it->second.atTime(t,allocate);
  }
 
  SX SymbolicOCP::atTime(const std::string& name, double t, bool allocate){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return it->second.atTime(t,allocate);
  }

  void SymbolicOCP::identifyALG(){
    // Quick return if no s
    if(this->s.empty()) return;

    // We investigate the interdependencies in sdot -> dae
    vector<SX> f_in;
    f_in.push_back(der(var(this->s)));
    SXFunction f(f_in,this->dae);
    f.init();

    // Number of s
    int ns = f.input().size();
    casadi_assert(f.output().size()==ns);

    // Input/output arrays
    bvec_t* f_sdot = reinterpret_cast<bvec_t*>(f.input().ptr());
    bvec_t* f_dae = reinterpret_cast<bvec_t*>(f.output().ptr());

    // First find out which equations depend on sdot
    f.spInit(true);
    
    // Seed all inputs
    std::fill(f_sdot,f_sdot+ns,bvec_t(1));

    // Propagate to f_dae
    std::fill(f_dae,f_dae+ns,bvec_t(0));
    f.spEvaluate(true);
    
    // Get the new differential and algebraic equations
    SX new_dae, new_alg;
    for(int i=0; i<ns; ++i){
      if(f_dae[i]==bvec_t(1)){
        new_dae.append(this->dae[i]);
      } else {
        casadi_assert(f_dae[i]==bvec_t(0));
        new_alg.append(this->dae[i]);
      }
    }

    // Now find out what sdot enter in the equations
    f.spInit(false);

    // Seed all outputs
    std::fill(f_dae,f_dae+ns,bvec_t(1));
    
    // Propagate to f_sdot
    std::fill(f_sdot,f_sdot+ns,bvec_t(0));
    f.spEvaluate(false);

    // Get the new algebraic variables and new states
    vector<Variable> new_s;
    SX new_z;
    for(int i=0; i<ns; ++i){
      if(f_sdot[i]==bvec_t(1)){
        new_s.push_back(this->s[i]);
      } else {
        casadi_assert(f_sdot[i]==bvec_t(0));
        new_z.append(this->s[i]->var_);
      }
    }

    // Make sure split was successful
    casadi_assert(new_dae.size()==new_s.size());
    
    // Divide up the s and dae
    s.clear();
    this->dae = new_dae;
    this->s = new_s;
    this->alg.append(new_alg);
    this->zQQQ.append(new_z);
  }

  std::vector<double> SymbolicOCP::nominal(const SX& var) const{
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::nominal: Argument must be a symbolic vector");
    std::vector<double> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = nominal(var.at(i).getName());
    }
    return ret;
  }

  std::vector<double> SymbolicOCP::min(const SX& var, bool nominal) const{
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::min: Argument must be a symbolic vector");
    std::vector<double> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = min(var.at(i).getName(),nominal);
    }
    return ret;
  }

  std::vector<double> SymbolicOCP::max(const SX& var, bool nominal) const{
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::max: Argument must be a symbolic vector");
    std::vector<double> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = max(var.at(i).getName(),nominal);
    }
    return ret;
  }

  std::vector<double> SymbolicOCP::start(const SX& var, bool nominal) const{
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::start: Argument must be a symbolic vector");
    std::vector<double> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = start(var.at(i).getName(),nominal);
    }
    return ret;
  }

  void SymbolicOCP::setStart(const SX& var, const std::vector<double>& val){
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::setStart: Argument must be a symbolic vector");
    casadi_assert_message(var.size()==var.size(),"SymbolicOCP::setStart: Dimension mismatch");
    for(int i=0; i<val.size(); ++i){
      setStart(var.at(i).getName(),val.at(i));
    }
  }

  std::vector<double> SymbolicOCP::initialGuess(const SX& var, bool nominal) const{
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::initialGuess: Argument must be a symbolic vector");
    std::vector<double> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = initialGuess(var.at(i).getName(),nominal);
    }
    return ret;
  }

  std::vector<double> SymbolicOCP::derivativeStart(const SX& var, bool nominal) const{
    casadi_assert_message(var.isVector() && var.isSymbolic(),"SymbolicOCP::derivativeStart: Argument must be a symbolic vector");
    std::vector<double> ret(var.size());
    for(int i=0; i<ret.size(); ++i){
      ret[i] = derivativeStart(var.at(i).getName(),nominal);
    }
    return ret;
  }

  std::string SymbolicOCP::unit(const std::string& name) const{
    map<string,Variable>::const_iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    return it->second->unit_;
  }

  std::string SymbolicOCP::unit(const SX& var) const{
    casadi_assert_message(!var.isVector() && var.isSymbolic(),"SymbolicOCP::unit: Argument must be a symbolic vector");
    if(var.isEmpty()){
      return "n/a";
    } else {
      string ret = unit(var.at(0).getName());
      for(int i=1; i<var.size(); ++i){
        casadi_assert_message(ret == unit(var.at(i).getName()),"SymbolicOCP::unit: Argument has mixed units");
      }
      return ret;
    }
  }

  void SymbolicOCP::setUnit(const std::string& name, const std::string& val){
    map<string,Variable>::iterator it = varmap_.find(name);
    casadi_assert_message(it!=varmap_.end(),"Variable \"" + name + "\" not found.");
    it->second->unit_ = val;
  }



} // namespace CasADi

