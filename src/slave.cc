/* 
  1.5D master/slave system
  
  DESCRIPTION: The slave receives all parameters and data from the master and performs whatever
               operation the master requests. The slave is prepared to work with different types 
	       atmospheres: Milne-Eddinton, LTE (own routines), non-LTE (RH)

  AUTHORS(s): Jaime de la Cruz Rodriguez, ISP-SU 2016

 */
#include <iostream>
#include <string>
#include <netcdf>
#include <mpi.h>
#include <vector>
#include <stdio.h>
#include "io.h"
#include "comm.h"
#include "input.h"
#include "atmosphere.h"
#include "clte.h"
#include "depthmodel.h"
#include "cmemt.h"
#include "crh.h"
//
#include "instruments.h"
#include "spectral.h"
#include "fpi.h"
#include "fpigen.h"
#include "specrebin.h"
#include "specprefilter.h"

using namespace std;
//
void do_slave(int myrank, int nprocs, char hostname[]){
  //
  string inam = "do_slave: ";
  iput_t input = {};
  vector<line_t> line;
  mat<double> w;
  
  //
  // Define ID and rank
  //
  input.myid = (string)"rank " +to_string(myrank)+ (string)", ";
  
  //
  // Get main parameters from master
  //
  input.nprocs = nprocs;
  MPI_Barrier(MPI_COMM_WORLD);// Wait until all processors reach this point
  comm_recv_parameters(input);
  
  
  
  input.myrank = myrank;
  if(input.mode == 1 || input.mode == 3) comm_send_weights(input, w);

  
  /* --- Init atmosphere --- */
  atmos *atmos;
  if(input.atmos_type == string("lte")){
    atmos = new clte(input, 4.44);
  }else if(input.atmos_type == string("rh")){
    atmos = new crh(input, 4.44);
  }else{
    cerr << input.myid << inam << "ERROR, atmos ["<<input.atmos_type<<"] not implemented"<<endl;
    exit(0);
  }


  /* --- (TO-DO, change this!) --- */

  vector<instrument*> inst;
  int nreg = atmos->input.regions.size();
  inst.resize(nreg);
  
  for(int kk = 0; kk<nreg; kk++){
    if(atmos->input.regions[kk].inst == "spectral") inst[kk] = new   spectral(atmos->input.regions[kk], 1);
    else if(atmos->input.regions[kk].inst == "fpi") inst[kk] = new       sfpi(atmos->input.regions[kk], 1);
    else if(atmos->input.regions[kk].inst == "fpigen") inst[kk] = new sfpigen(atmos->input.regions[kk], 1);
    else if(atmos->input.regions[kk].inst == "specrebin") inst[kk] = new specrebin(atmos->input.regions[kk], 1);
    else if(atmos->input.regions[kk].inst == "specprefilter") inst[kk] = new specprefilter(atmos->input.regions[kk], 1);
    else inst[kk] = new instrument();
  }
  atmos->inst = &inst[0];

  
  vector<mdepth_t> m;
  mat<double> dobs;
  vector<double> pgas_saved;
  pgas_saved.resize(input.ndep);
  
  // 
  // Work until action == 0
  //
  while(1){
    int action = 0;
    mat<double> obs, pars;


    //
    // Receive package from master, including action
    //
    int compute_derivatives = 0;
    comm_slave_unpack_data(input, action, obs, pars, m, compute_derivatives);
    if(action == 0) break; // Exit while loop if action = 0
    
    //
    // Execute action depending on input.mode 
    //
    if(input.mode == 1){

      /* --- Invert pixels --- */
      for(int pp = 0; pp<input.nPacked; pp++){

	/* --- Update instrumental profile if needed --- */
	
	for(int kk = 0; kk<nreg; kk++) inst[kk]->update(input.regions[kk].psf.d.size(), &input.regions[kk].psf.d[0]);

	
	/* --- Perform inversion --- */
	
	input.chi[pp] =
	  atmos->fitModel2( m[pp], input.npar, &pars(pp,0),
			    (int)(input.nw_tot*input.ns), &obs(pp,0,0), w);
      }

      
      // Send back to master
      
      comm_slave_pack_data(input, obs, pars, dobs, compute_derivatives, m);
      
    }else if(input.mode == 2){
      
      
      /* --- Allocate vars to store the response function --- */
      
      int nPacked = input.nPacked;
      int ndata = input.nw_tot * input.ns;
      vector<int> mydims = {nPacked, input.npar, input.nw_tot, input.ns};

      
      /* --- Loop pixels --- */
      int pixel = 0;      
      for(auto &it: m){

	/* --- Log tau to tau --- */
	
	for(int kk = 0; kk < input.ndep; kk++)
	  it.tau[kk] = pow(10.0, it.ltau[kk]); 
	

	
	/* --- Call equation of state or hydrostatic equilibrium ? --- */
	if(input.use_eos){
	  if(input.thydro) it.getPressureScale(input.nodes.depth_t, input.boundary, *(atmos->eos));
	  else{
	    it.fill_densities(*(atmos->eos), input.keep_nne, 0, it.ndep-1);
	
	/* --- Get scales (depth_t has cmass and z switched compared to getScales) --- */
	
	    if     (input.nodes.depth_t == 0) it.getScales(*atmos->eos, 0); // LTAU500
	    else if(input.nodes.depth_t == 1) it.getScales(*atmos->eos, 2); // CMASS
	    else if(input.nodes.depth_t == 2) it.getScales(*atmos->eos, 1); // Z
	  }
	}
	
	/* --- Optimize depth scale? --- */
	
	if(input.tcut > 0)
	  it.optimize_depth(*(atmos->eos), input.tcut, 11);
	

	
	
	
	/* --- Synthesize spectra --- */
	  
	bool conv = atmos->synth(it, &obs(pixel,0,0), 0, (cprof_solver)input.solver);
	atmos->cleanup();
	
	/* --- If not converged, printout message --- */
	if(!conv) {
	  int x =0, y=0;
	  comm_get_xy(input.ipix+pixel, input.nx, y, x);
	  
	  fprintf(stderr, "[%6d] slave: ERROR, atom populations did not converge for pixel (x,y) = [%4d,%4d]\n", myrank, x, y);
	}
      
	/* --- Update instrumental profile if needed --- */
	
	for(int kk = 0; kk<nreg; kk++) //inst[kk]->update((size_t)(input.ipix + pixel));
	  inst[kk]->update(input.regions[kk].psf.d.size(), &input.regions[kk].psf.d[0]);

	
	/* --- Degrade --- */

	atmos->spectralDegrade(input.ns, (int)1, ndata, &obs(pixel, 0, 0));
	
	pixel++;
      }



      
      
      /* --- Send back profiles --- */
      
      comm_slave_pack_data(input, obs, pars, dobs, compute_derivatives, m);
      m.clear();
      
      
    }else if(input.mode == 3){

      /* --- Allocate vars to store the response function --- */
      int nPacked = input.nPacked;
      int ndata = input.nw_tot * input.ns;
      vector<int> mydims = {nPacked, input.npar, input.nw_tot, input.ns};
      dobs.set(mydims);
      dobs.zero();
      
      
      /* --- Loop pixels --- */
      int pixel = 0;      
      for(auto &it: m){


	/* --- Check parameter ranges --- */
	for(int pp = 0; pp<input.npar; pp++)
	  pars(pixel,pp) = atmos->checkParameter(pars(pixel,pp), pp);
	
	/* --- Expand the nodes into a depth stratified atmosphere --- */
	
	it.expand(input.nodes, &pars(pixel,0), input.dint, input.depth_model);
	atmos->checkBounds(it);

	
	/* --- Log tau to tau --- */
	
	for(int kk = 0; kk < input.ndep; kk++)
	  it.tau[kk] = pow(10.0, it.ltau[kk]); 

	/* --- get pressure scale assuming hydrostatic eq. --- */
	it.getPressureScale(input.nodes.depth_t, input.boundary, *(atmos->eos)); // Hydrostatic eq. to derive pressure scale
	//it.nne_enhance(input.nodes, input.npar, &pars(pixel,0), atmos->eos);

	memcpy(&pgas_saved[0], &it.pgas[0], input.ndep*sizeof(double)); // Store pgas

	
	/* --- Synthesize spectra --- */
	atmos->synth(it, &obs(pixel,0,0), 0, (cprof_solver)input.solver);

	
	if(compute_derivatives){
	  /* --- Compute response function with centered derivatives 
	     invert loop because the same height scale can be used for all parameters
	     except for temperature that is packed at the beginning ... do them at
	     the end! --- */
	  for(int nn = input.npar-1; nn>=0; nn--)
	    atmos->responseFunction(input.npar, it, &pars(pixel,0),
				    ndata, &dobs(pixel,nn,0,0), nn, &obs(pixel,0,0));
	  
	} // compute derivatives

	memcpy(&it.pgas[0], &pgas_saved[0], input.ndep*sizeof(double));
	
	pixel++;
      } // auto it: m
      
      
      /* --- Send results back to master --- */
      comm_slave_pack_data(input, obs, pars, dobs, compute_derivatives, m);

      
      /* --- Clean-up ---*/
      dobs.set({0,0});
      m.clear();
      
    }else if(input.mode == 4){
      
      
      /* --- Allocate vars to store the response function --- */
      
      int nPacked = input.nPacked;
      int ndata = input.nw_tot * input.ns;
      vector<int> mydims = {nPacked, input.nresp, input.ndep, input.nw_tot * input.ns};
      
      dobs.set(mydims);
      dobs.zero();
      
      /* --- Loop pixels --- */
      int pixel = 0;      
      for(auto &it: m){

	/* --- Log tau to tau --- */
	
	for(int kk = 0; kk < input.ndep; kk++)
	  it.tau[kk] = pow(10.0, it.ltau[kk]); 
	
	
	/* --- Call equation of state or hydrostatic equilibrium ? --- */
	
	if(input.use_eos){
	  if(input.thydro) it.getPressureScale(input.nodes.depth_t, input.boundary, *(atmos->eos));
	  else it.fill_densities(*(atmos->eos), input.keep_nne, 0, it.ndep-1);
	}
	
	/* --- Optimize depth scale? --- */
	
	if(input.tcut > 0)
	  it.optimize_depth(*(atmos->eos), input.tcut, 7);

	
	/* --- Update instrumental profile if needed --- */
	
	for(int kk = 0; kk<nreg; kk++) //inst[kk]->update((size_t)(input.ipix + pixel));
	  inst[kk]->update(input.regions[kk].psf.d.size(), &input.regions[kk].psf.d[0]);

	
	/* --- Synthesize spectra --- */
	
	bool conv = atmos->synth(it, &obs(pixel,0,0), 0, (cprof_solver)input.solver, true);
	
	/* --- If not converged, printout message --- */
	if(!conv) {
	  int x =0, y=0;
	  comm_get_xy(input.ipix+pixel, input.nx, y, x);
	  
	  fprintf(stderr, "[%6d] slave: ERROR, atom populations did not converge for pixel (x,y) = [%4d,%4d]\n", myrank, x, y);
	}

	// --- Derivatives ---- //
	
	int kkk = 0;
	for(int kk = 0;kk<8; kk++){
	  if(input.getResponse[kk] > 0){
	    atmos->responseFunctionFull(it, ndata, &dobs(pixel,kkk,0,0), &obs(pixel,0,0), kk);
	    
	    for(int zz = 0; zz<it.ndep; zz++)
	      atmos->spectralDegrade(input.ns, (int)1, ndata, &dobs(pixel, kkk, zz, 0));
	    
	    kkk++;
	  }
	}
	
	atmos->cleanup();

	
	/* --- Degrade --- */

	atmos->spectralDegrade(input.ns, (int)1, ndata, &obs(pixel, 0, 0));
	pixel++;
      }

      
      
      /* --- Send back profiles --- */
      
      comm_slave_pack_data(input, obs, pars, dobs, compute_derivatives, m);
      m.clear();
      
      
    } // case 4


    m.clear();
    
  }

  
  for(auto &it: inst)
    delete it;  
  
}
