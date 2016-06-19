#include <vector>
#include <string>
#include <iostream>
#include <cmath>
#include "depthmodel.h"
#include "interpol.h"
#include "ceos.h"
#include "physical_consts.h"

using namespace std;

/* --- Default boundary condition (if none provided) --- */
const double mdepth::boundary_pgas_default = 1.e-1;

void mdepth::setsize(int n){

  /* --- resize --- */
  cub.set({14, n});
  ndep = n;
  
  /* --- Assign pointers (keep this order so we can 
     copy the buffer directly ---*/
  
  temp =  &cub(0,0);
  v =     &cub(1,0);
  vturb = &cub(2,0);
  b =     &cub(3,0);
  inc =   &cub(4,0);
  azi =   &cub(5,0);
  pgas =  &cub(6,0);
  rho  =  &cub(7,0);
  nne  =  &cub(8,0);
  ltau =  &cub(9,0);
  z =     &cub(10,0);
  cmass = &cub(11,0);
  pel =   &cub(12,0);
  tau  =  &cub(13,0);
}

void mdepth::zero(){
  cub.zero();
}
mdepth& mdepth::operator= (  mdepth &m)
{

  this->setsize(m.ndep);
  memcpy(&(this->cub(0,0)), &m.cub(0,0), 14*m.ndep*sizeof(double));

  return *this;
}

void mdepth::nodes2depth(int n, double *x, double *y, int nn, double *xx, double *yy, int interpol)
{
  if     (n <  1)           return;
  else if(n == 1)           for(int kk=0; kk<nn; kk++) yy[kk] = y[0];
  else if(n == 2)            linpol<double,double>(n, x, y, nn, xx, yy, true);
  else if(n >= 3){
    if     (interpol == 0)   linpol<double,double>(n, x, y, nn, xx, yy, true);
    else if(interpol == 1)  bezpol2<double,double>(n, x, y, nn, xx, yy, true);
    else                    hermpol<double,double>(n, x, y, nn, xx, yy, true);
  }
}

void mdepth::nne_enhance(nodes_t &nodes, int n, double *pars, ceos &eos){

  /* --- are we inverting the nne enhancement? --- */
  
  bool doit = false;
  double mult = 1.0;
  //
  for(int ii = 0; ii < nodes.nnodes; ii++)
    if(nodes.ntype[ii] == pgas_node){
      doit = true;
      mult = pars[ii];
    }
  if(!doit) return;

  //fprintf(stderr,"   [mult=%f]\n", mult);
  
  /* --- Enhance electron pressure from ltau_500 <= -4.5 --- */

  const double dx = 0.2, x0 = -4.8;
  
  for(int ii = 0; ii<ndep; ii++){
    double at = -tanh((ltau[ii]-x0)*phyc::PI/(dx)) * 0.5 + 0.5;
    double corr = mult * at + (1.0 - at);
    nne[ii] *= corr;

    eos.nne_from_T_Pg_nne (temp[ii], pgas[ii],  rho[ii], nne[ii]);
    eos.store_partial_pressures(ndep, ii, eos.xna, eos.xne);
  }

}

void mdepth::expand(nodes_t &n, double *p, int interpol){
  
  // int ndep = (int)cub.size(1);
  
  if(n.toinv[0]){
    int len = (int)n.temp.size();
    nodes2depth(len, &n.temp[0], &p[n.temp_off], ndep, &ltau[0], &temp[0], interpol);
  }
  
  if(n.toinv[1]){
    int len = (int)n.v.size();
    nodes2depth(len, &n.v[0], &p[n.v_off], ndep, &ltau[0], &v[0], interpol);
  }
  
  if(n.toinv[2]){
    int len = (int)n.vturb.size();
    nodes2depth(len, &n.vturb[0], &p[n.vturb_off], ndep, &ltau[0], &vturb[0], interpol);
  }
  
  if(n.toinv[3]){
    int len = (int)n.b.size();
    nodes2depth(len, &n.b[0], &p[n.b_off], ndep, &ltau[0], &b[0], interpol);
  }
  
  if(n.toinv[4]){
    int len = (int)n.inc.size();
    nodes2depth(len, &n.inc[0], &p[n.inc_off], ndep, &ltau[0], &inc[0], interpol);
  }
  
  if(n.toinv[5]){
    int len = (int)n.azi.size();
    nodes2depth(len, &n.azi[0], &p[n.azi_off], ndep, &ltau[0], &azi[0], interpol);
  }
  /*
  if(n.toinv[6]){
    fprintf(stderr, "bound=%1d, val=%e, multi=%e\n", n.bound, bound_val, p[n.pgas_off]);
    if     (n.bound == 1) pgas[0] = bound_val*p[n.pgas_off];
    else if(n.bound == 2) rho[0]  = bound_val*p[n.pgas_off];
    else if(n.bound == 3) nne[0]  = bound_val*p[n.pgas_off];
    else pgas[0] =  boundary_pgas_default*p[n.pgas_off];
  }//else fprintf(stderr, "bound=%1d, val=%e\n", n.bound, bound_val);

  */
  
  return;
}

void mdepth::fill_densities(ceos &eos, int keep_nne){

  /* --- which scale do we have? --- */
  
  int bound = -1;
  double spgas = 0.0, srho = 0.0, snne = 0.0;

  for(int kk = 0; kk<ndep; kk++){
    spgas += pgas[kk];
    srho  += rho[kk];
    snne  += nne[kk];
  }

  if(spgas > 0.0) bound = 0;
  else if(srho > 0.0) bound = 1;
  else if(snne > 0.0) bound = 3;
  

  /* --- Fill the density arrays, the partial pressures are stored internally
     inside fill_densities --- */
  
  eos.fill_densities(ndep, temp, pgas, rho, pel, nne, bound,  keep_nne, 1.e-5);



  /* --- Get scales --- */

  double sz = 0.0, stau = 0.0, scm = 0.0;
  for(int kk = 0; kk<ndep; kk++){
    sz += fabs(z[kk]);
    stau += fabs(ltau[kk]);
    scm += fabs(cmass[kk]);
  }

  bound = -1;
  if(stau > 0.0)     bound = 0;
  else if(sz > 0.0)  bound = 1;
  else if(scm > 0.0) bound = 2;


  /* --- Fill scales --- */

  getScales(eos, bound);
  
  
}

void mdepth::getScales(ceos &eos, int bound){

  vector<double> kappa;
  kappa.resize(ndep);
  int nw = 1;
  double wav = 5000.0, scat = 0.0;
  vector<float> frac, part;
  float na=0, ne=0;

  
  /* --- get cont opac --- */
  
  for(int k=0; k<ndep; k++){
    eos.read_partial_pressures(0, frac, part, na, ne);
    eos.contOpacity(temp[k], nw,  &wav, &kappa[k],
			      &scat, frac, na, ne);
  }
  
  
  if(bound == 0){  /* --- if we know tau --- */


    tau[0] = pow(10.0, ltau[0]);
    cmass[0] = (tau[0] / kappa[0]) * rho[0];
    z[0] = 0.0;
    
    for(int k = 1; k < ndep; k++){
      tau[k] = pow(10.0, ltau[k]);
      z[k] = z[k-1] - 2.0 * (tau[k] - tau[k-1]) / (kappa[k] + kappa[k-1]);
      cmass[k] = cmass[k-1] + 0.5*(rho[k-1] + rho[k]) * (z[k-1] - z[k]);
    } // k
  }else if(bound == 1){  /* --- if we know Z --- */

    eos.read_partial_pressures(0, frac, part, na, ne);
    tau[0] = 0.5 * kappa[0] * (z[0] - z[1]);
    cmass[0] = (na + ne) * (phyc::BK * temp[0] / eos.gravity);
    ltau[0] = log10(tau[0]);

    for(int k = 1; k < ndep; k++){
      tau[k] = tau[k-1] + 0.5 * (kappa[k-1] + kappa[k]) * (z[k-1] - z[k]);
      cmass[k] = cmass[k-1] + 0.5 * (rho[k-1] + rho[k]) * (z[k-1] - z[k]);
      ltau[k] = log10(tau[k]);
    }
    

  }else if(bound ==2){ /* --- If we know cmass --- */
    
    z[0] = 0.0;
    tau[0] = 0.0; //kappa[0]/rho[0] * cmass[0];
    
    
    for(int k = 1; k < ndep; k++){
      z[k] = z[k-1] - 2.0 * (cmass[k] - cmass[k-1]) / (rho[k-1] + rho[k]);
      tau[k] = tau[k-1] + 0.5 * (kappa[k-1] + kappa[k]) * (z[k-1] - z[k]);
    }
    
    /* --- Extrapolate tau at the top --- */
    
    double toff = exp(2.0 * log(tau[1]) - log(tau[2]));
    
    for(int k = 0; k < ndep; k++){
      tau[k] = log10(tau[k]+toff);
    } 
  }
  
}


void mdepth::fixBoundary(int boundary, ceos &eos){
  
  if(boundary == 0) pgas[0] = boundary_pgas_default;
  else if(boundary == 1) nne[0] = eos.nne_from_T_Pg(temp[0], pgas[0],  rho[0]);
  else if(boundary == 2) nne[0] = eos.nne_from_T_rho(temp[0], pgas[0], rho[0]);
  else if(boundary == 3) rho[0] = eos.rho_from_T_nne(temp[0], pgas[0], nne[0]);
  else                   rho[0] = eos.rho_from_T_pel(temp[0], pgas[0], pel[0]);
  
}

void mdepth::getPressureScale(int boundary, ceos &eos){

  /* --- If pgas was not given, convert rho or nne or pel to pgas --- */
  
  fixBoundary(boundary, eos);

  /* --- Solve hydrostatic eq. --- */
  
  eos.hydrostatic((int)ndep, &tau[0], &temp[0], &pgas[0], &rho[0], &nne[0], &pel[0],
		  &z[0], &cmass[0], pgas[0], (float)1.e-5);
}


void mdepthall::setsize(int ny, int nx, int ndep, bool verbose){
  std::string inam = "mdepthall::setsize: ";

  std::vector<int> dims = {ny, nx, ndep};
  if(verbose) std::cout << inam << "["<<dims[0]<<" "<<dims[1]<<" "<<dims[2]<<"]"<<std::endl;
		 

  cub.set({ny, nx, 13, ndep});
}


void mdepthall::compress(int n, double *x, double *y, int nn, double *xx, double *yy){
  if(nn == 0) return;
  else if(nn == 1){
    double tmp = 0.0;
    for(int zz = 0; zz<n;zz++) tmp += y[zz];
    yy[0] = tmp / (double)n;
    return;
  } else {
    linpol<double, double>(n, x, y, nn, xx, yy, false);
    return;
  }
}
void mdepthall::compress(int n, float *x, float *y, int nn, double *xx, double *yy){
  if(nn == 0) return;
  else if(nn == 1){
    double tmp = 0.0;
    for(int zz = 0; zz<n;zz++) tmp += y[zz];
    yy[0] = tmp / (double)n;
    return;
  } else {
    linpol<float, double>(n, x, y, nn, xx, yy, true);
    return;
  }
}

void mdepthall::model_parameters(mat<double> &tmp, nodes_t &n, int nt){

  int nnodes = n.nnodes;
  int nx = temp.size(1);
  int ny = temp.size(0);
  int ndep = temp.size(2);
    
  tmp.set({ny, nx, nnodes});
    
  for(int yy = 0; yy < ny; yy++)
    for(int xx = 0; xx < nx; xx++){

      int k = 0;
      int nn = 0;
	
      // Temp
      nn = (int)n.temp.size();
      compress(ndep, &ltau(yy,xx,0), &temp(yy,xx,0), nn, &n.temp[0], &tmp(yy,xx,k));
      k += nn;

      // v_los
      nn = (int)n.v.size();
      compress(ndep, &ltau(yy,xx,0), &v(yy,xx,0), nn, &n.v[0], &tmp(yy,xx,k));
      k += nn;

      // vturb
      nn = (int)n.vturb.size();
      compress(ndep, &ltau(yy,xx,0), &vturb(yy,xx,0), nn, &n.vturb[0], &tmp(yy,xx,k));
      k += nn;

      // B
      nn = (int)n.b.size();
      compress(ndep, &ltau(yy,xx,0), &b(yy,xx,0), nn, &n.b[0], &tmp(yy,xx,k));
      k += nn;
	
      // Inc
      nn = (int)n.inc.size();
      compress(ndep, &ltau(yy,xx,0), &inc(yy,xx,0), nn, &n.inc[0], &tmp(yy,xx,k));
      k += nn;
      
      // azi
      nn = (int)n.azi.size();
      compress(ndep, &ltau(yy,xx,0), &azi(yy,xx,0), nn, &n.azi[0], &tmp(yy,xx,k));
      k += nn;

      //	for(int zz = 0; zz < ndep; zz++) ivar()
    }

}

void mdepthall::model_parameters2(mat<double> &tmp, nodes_t &n, int nt){

  int nnodes = n.nnodes;
  int nx = cub.size(1);
  int ny = cub.size(0);
  int ndep = cub.size(3);
    
  tmp.set({ny, nx, nnodes});
    
  for(int yy = 0; yy < ny; yy++)
    for(int xx = 0; xx < nx; xx++){

      int k = 0;
      int nn = 0;
	
      // Temp
      nn = (int)n.temp.size();
      compress(ndep, &cub(yy,xx,9,0), &cub(yy,xx,0,0), nn, &n.temp[0], &tmp(yy,xx,k));
      k += nn;

      // v_los
      nn = (int)n.v.size();
      compress(ndep, &cub(yy,xx,9,0), &cub(yy,xx,1,0), nn, &n.v[0], &tmp(yy,xx,k));
      k += nn;

      // vturb
      nn = (int)n.vturb.size();
      compress(ndep, &cub(yy,xx,9,0), &cub(yy,xx,2,0), nn, &n.vturb[0], &tmp(yy,xx,k));
      k += nn;

      // B
      nn = (int)n.b.size();
      compress(ndep, &cub(yy,xx,9,0), &cub(yy,xx,3,0), nn, &n.b[0], &tmp(yy,xx,k));
      k += nn;
	
      // Inc
      nn = (int)n.inc.size();
      compress(ndep, &cub(yy,xx,9,0), &cub(yy,xx,4,0), nn, &n.inc[0], &tmp(yy,xx,k));
      k += nn;
      
      // azi
      nn = (int)n.azi.size();
      compress(ndep, &cub(yy,xx,9,0), &cub(yy,xx,5,0), nn, &n.azi[0], &tmp(yy,xx,k));
      k += nn;

      // Pgas boundary
      
      if(n.toinv[6]>0){
	tmp(yy,xx,k) = 1.0;
      }
      
    }

}


int mdepthall::read_model2(std::string &filename, int tstep, bool require_tau){
  io ifile(filename, netCDF::NcFile::read);
  std::string inam = "mdepthall::read_model: ";
  int idep, bound = 0;
  mat<double> tmp;
    
  /* --- get dimensions --- */
  std::vector<int> dims = ifile.dimSize("temp");
  int ndims = (int)dims.size();

  
  if(ndims == 4) dims.erase(dims.begin()); // if time, remove first element
  else if(ndims != 3) {
    std::cout << inam << "ERROR, ndims must be 3 or 4: [(nt), ny, nx, ndep], but is "<<ndims<<std::endl;
  }     

  /* --- Allocate cube --- */
  cub.set({dims[0], dims[1], 13, dims[2]});
  ndep = dims[2];

  
  /* --- read vars, assuming they exists --- */
  if(ifile.is_var_defined("temp")){
    ifile.read_Tstep<double>("temp", tmp, tstep);

    /* --- Copy to consecutive arary --- */
    for(int yy = 0; yy< dims[0]; yy++)
      for(int xx = 0; xx < dims[1]; xx++)
	memcpy(&cub(yy,xx,0,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
  }else {
    std::cerr << inam << "ERROR, "<<filename <<" does not contain a temperature array, exiting"<<std::endl;
    exit(0);
  }

  boundary.set({dims[0], dims[1]});


  /* --- Read Vlos --- */
  if(ifile.is_var_defined("vlos")){
    ifile.read_Tstep<double>("vlos", tmp, tstep);
     for(int yy = 0; yy< dims[0]; yy++)
      for(int xx = 0; xx < dims[1]; xx++)
	memcpy(&cub(yy,xx,1,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
  }
  

  
  /* --- Read Vmic --- */
   if(ifile.is_var_defined("vturb")){
    ifile.read_Tstep<double>("vturb", tmp, tstep);
    for(int yy = 0; yy< dims[0]; yy++)
      for(int xx = 0; xx < dims[1]; xx++)
	memcpy(&cub(yy,xx,2,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }



   /* --- Read B --- */
   if(ifile.is_var_defined("b")){
     ifile.read_Tstep<double>("b", tmp, tstep);
     for(int yy = 0; yy< dims[0]; yy++)
       for(int xx = 0; xx < dims[1]; xx++)
	 memcpy(&cub(yy,xx,3,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }
   
   


   /* --- Read inc --- */
   if(ifile.is_var_defined("inc")){
    ifile.read_Tstep<double>("inc", tmp, tstep);
    for(int yy = 0; yy< dims[0]; yy++)
       for(int xx = 0; xx < dims[1]; xx++)
	 memcpy(&cub(yy,xx,4,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }




   /* --- Read azi --- */
   if(ifile.is_var_defined("azi")){
     ifile.read_Tstep<double>("azi", tmp, tstep);
     for(int yy = 0; yy< dims[0]; yy++)
       for(int xx = 0; xx < dims[1]; xx++)
	 memcpy(&cub(yy,xx,5,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }
   



   /* --- Read Pgas --- */
   if(ifile.is_var_defined("pgas")){
     ifile.read_Tstep<double>("pgas", tmp, tstep);
     for(int yy=0; yy<dims[0]; yy++) for(int xx = 0;xx<dims[1]; xx++){
	 boundary(yy,xx) = tmp(yy,xx,0);
	 memcpy(&cub(yy,xx,6,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
       }
   }


 
   
   /* --- Read Rho --- */
   if(ifile.is_var_defined("rho")){
     ifile.read_Tstep<double>("rho", tmp, tstep);
     if(bound == 0) {
       for(int yy=0; yy<dims[0]; yy++)
	 for(int xx = 0;xx<dims[1]; xx++){
	   boundary(yy,xx) = tmp(yy,xx,0);
	 }
     }
     
     for(int yy=0; yy<dims[0]; yy++)
       for(int xx = 0;xx<dims[1]; xx++)
	 memcpy(&cub(yy,xx,7,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }



   
   /* --- Read nne --- */
   if(ifile.is_var_defined("nne")){
     ifile.read_Tstep<double>("nne", tmp, tstep);
     if(bound == 0) {
       for(int yy=0; yy<dims[0]; yy++)
	 for(int xx = 0;xx<dims[1]; xx++){
	   boundary(yy,xx) = tmp(yy,xx,0);
	 }
     }
     
     for(int yy=0; yy<dims[0]; yy++)
       for(int xx = 0;xx<dims[1]; xx++)
	 memcpy(&cub(yy,xx,8,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }


   /* --- Init boundary --- */
   if(fabs(cub(0,0,6,0)) > 0.0) bound = 1;
   else if(fabs(cub(0,0,7,0)) > 0.0) bound = 2;
   else if(fabs(cub(0,0,8,0)) > 0.0) bound = 3;

   if(bound > 0)
     for(int yy=0; yy<dims[0]; yy++)
       for(int xx = 0;xx<dims[1]; xx++)
	 boundary(yy,xx) = cub(yy,xx,5+bound, 0);
   cerr<<"mdepthall::read_model2: Bound -> "<<bound<<endl;
   


   /* --- Read LTAU500 --- */
   bool set_ltau = false;
   if(ifile.is_var_defined("ltau500")){
    ifile.read_Tstep<double>("ltau500", tmp, tstep);
    set_ltau = true;
    for(int yy=0; yy<dims[0]; yy++)
      for(int xx = 0;xx<dims[1]; xx++)
	memcpy(&cub(yy,xx,9,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }



   /* --- Read Z --- */
   bool set_z = false;
   if(ifile.is_var_defined("z")){
     ifile.read_Tstep<double>("z", tmp, tstep);
     set_z = true;
     if(tmp.ndims() == 1){
       cerr<< inam <<"replicating z-scale in all pixels"<<endl;
       for(int yy = 0; yy<dims[0];yy++)
	 for(int xx = 0; xx< dims[1]; xx++)
	   for(int zz = 0; zz<dims[2]; zz++)
	     cub(yy,xx,10,zz) = tmp.d[zz];
     }else{
       for(int yy=0; yy<dims[0]; yy++)
	 for(int xx = 0;xx<dims[1]; xx++)
	   memcpy(&cub(yy,xx,10,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
       
     }
     
   }
   
   /* -- Read cmass? --- */
   
   if(ifile.is_var_defined("cmass")){
     ifile.read_Tstep<double>("cmass", tmp, tstep);
     for(int yy=0; yy<dims[0]; yy++)
       for(int xx = 0;xx<dims[1]; xx++)
	 memcpy(&cub(yy,xx,11,0), &tmp(yy,xx,0), dims[2]*sizeof(double));
   }

   
   if(set_ltau == false){
    std::cerr << inam << "ERROR, "<<filename
	      <<" does not contain a depth-scale [ltau500], exiting"
	      <<std::endl;
    exit(0);
  }
   

   return bound;
}

void mdepthall::convertBoundary(int bound, bool verbose){

  string inam = "depthall::convertBoundary: ";
  
  vector<int> dims = boundary.getdims();
  
  if(verbose) cout << inam << "converting boundary to Pgas if needed ... ";
  switch(bound)
    {
    case(2):
      for(int yy=0;yy<dims[0];yy++){
	cerr << yy <<" ";
	for(int xx=0;xx<dims[1];xx++){
	  boundary(yy,xx) = eos.nne_from_T_rho(temp(yy,xx,0), pgas(yy,xx,0), rho(yy,xx,0));
	}
      }
      break;
    case(3):
      for(int yy=0;yy<dims[0];yy++)
	for(int xx=0;xx<dims[1];xx++)
	  boundary(yy,xx) = eos.nne_from_T_rho(temp(yy,xx,0), pgas(yy,xx,0), nne(yy,xx,0));
      break;
    case(4):
      for(int yy=0;yy<dims[0];yy++)
	for(int xx=0;xx<dims[1];xx++)
	  boundary(yy,xx) = eos.nne_from_T_rho(temp(yy,xx,0), pgas(yy,xx,0), pel(yy,xx,0));
      break;
    defaul:
      break;
    }
  
  if(verbose) cout << endl;
  
}

void mdepthall::expand(int n, double *x, double *y, int nn, double *xx, double *yy, int interpolation){

  if     (n == 1)                for(int kk=0;kk<nn;kk++) yy[kk] = y[0];
  else if((n == 2))              linpol<double,double>(n, x, y, nn, xx, yy, true);
  else if(n >= 3){
    if(interpolation == 0)       linpol<double,double>(n, x, y, nn, xx, yy, true);
    else if(interpolation == 1) bezpol2<double,double>(n, x, y, nn, xx, yy, true);
    else                        hermpol<double,double>(n, x, y, nn, xx, yy, true);
  }
  else return;
}


void mdepthall::expandAtmos(nodes_t &n, mat<double> &pars, int interpolation){

  int ny = pars.size(0);
  int nx = pars.size(1);

  for(int yy = 0; yy< ny; yy++) for(int xx=0;xx<nx;xx++){

      /* --- temperature --- */
      if(n.toinv[0]){
	int len = (int)n.temp.size();
	expand(len, &n.temp[0], &pars(yy,xx,n.temp_off), ndep, &cub(yy,xx,9,0), &cub(yy,xx,0,0), interpolation);
      }


      /* --- vlos --- */
      if(n.toinv[1]){
	int len = (int)n.v.size();
	expand(len, &n.v[0], &pars(yy,xx,n.v_off), ndep, &cub(yy,xx,9,0), &cub(yy,xx,1,0), interpolation);
      }


      /* --- vturb --- */
      if(n.toinv[2]){
	int len = (int)n.vturb.size();
	expand(len, &n.vturb[0], &pars(yy,xx,n.vturb_off), ndep, &cub(yy,xx,9,0), &cub(yy,xx,2,0), interpolation);
      }

      
      /* --- B --- */
      if(n.toinv[3]){
	int len = (int)n.b.size();
	expand(len, &n.b[0], &pars(yy,xx,n.b_off), ndep, &cub(yy,xx,9,0), &cub(yy,xx,3,0), interpolation);
      }


      /* --- Inc --- */
      if(n.toinv[4]){
	int len =(int)n.inc.size();
	expand(len, &n.inc[0], &pars(yy,xx,n.inc_off), ndep, &cub(yy,xx,9,0), &cub(yy,xx,4,0), interpolation);
      }



      /* --- Azi --- */
      if(n.toinv[5]){
	int len = (int)n.azi.size();
	expand(len, &n.azi[0], &pars(yy,xx,n.azi_off), ndep, &cub(yy,xx,9,0), &cub(yy,xx,5,0), interpolation);
      }
      
    } // xx & yy
  
  
}


void mdepthall::write_model(string &filename, int tstep){

  static bool firsttime = true;

  /* --- init output file ont he first call --- */
  static io ofile(filename, netCDF::NcFile::replace);


  /* --- Init vars & dims if firsttime --- */
  if(firsttime){
    /* --- Dims --- */
    vector<int> dims = temp.getdims();
    dims.insert(dims.begin(), 0);
    ofile.initDim({"time","y", "x", "ndep"}, dims);

    /* --- vars -- */
     ofile.initVar<float>(string("temp"),    {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("vlos"),    {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("vturb"),   {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("b"),       {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("inc"),     {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("azi"),     {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("ltau500"), {"time","y", "x", "ndep"});
     ofile.initVar<float>(string("pgas"),    {"time","y", "x", "ndep"});

     firsttime = false;
    
  }


  /* --- write time stamp --- */
  ofile.write_Tstep(string("temp"),    temp,  tstep);
  ofile.write_Tstep(string("vlos"),    v,     tstep);
  ofile.write_Tstep(string("vturb"),   vturb, tstep);
  ofile.write_Tstep(string("b"),       b,     tstep);
  ofile.write_Tstep(string("inc"),     inc,   tstep);
  ofile.write_Tstep(string("azi"),     azi,   tstep);
  ofile.write_Tstep(string("ltau500"), ltau,  tstep);
  ofile.write_Tstep(string("pgas"),    pgas,  tstep);

  
}



void mdepthall::write_model2(string &filename, int tstep){

  static bool firsttime = true;

  /* --- init output file ont he first call --- */
  
  static io ofile(filename, netCDF::NcFile::replace);

  
  /* --- Dims --- */
  vector<int> cdims = cub.getdims();
  vector<int> dims = {0, cdims[0], cdims[1], cdims[3]};


  
  /* --- Init vars & dims if firsttime --- */
  if(firsttime){

    ofile.initDim({"time","y", "x", "ndep"}, dims);

    
    /* --- vars -- */
    
    ofile.initVar<float>(string("vlos"),    {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("temp"),    {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("vturb"),   {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("b"),       {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("inc"),     {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("azi"),     {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("ltau500"), {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("z"),       {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("pgas"),    {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("rho"),     {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("nne"),     {"time","y", "x", "ndep"});
    ofile.initVar<float>(string("cmass"),     {"time","y", "x", "ndep"});

    firsttime = false;
    
  }
  

  mat<double> tmp((vector<int>){dims[1], dims[2], dims[3]});
  
  /* --- write time step --- */
  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,0,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("temp"),    tmp,  tstep);

  
  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,1,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("vlos"),   tmp,  tstep);

  
  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,2,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("vturb"),   tmp, tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,3,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("b"),       tmp,     tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,4,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("inc"),     tmp,   tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,5,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("azi"),     tmp,   tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,9,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("ltau500"), tmp,  tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,10,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("z"), tmp,  tstep);

  
  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,6,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("pgas"),    tmp,  tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,7,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("rho"),    tmp,  tstep);

  
  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,8,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("nne"),    tmp,  tstep);

  for(int yy=0; yy<dims[1]; yy++)
    for(int xx = 0;xx<dims[2];xx++)
      memcpy(&tmp(yy,xx,0), &cub(yy,xx,11,0), dims[3]*sizeof(double));
  ofile.write_Tstep<double>(string("cmass"),    tmp,  tstep);


  
}
