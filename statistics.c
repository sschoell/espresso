// This file is part of the ESPResSo distribution (http://www.espresso.mpg.de).
// It is therefore subject to the ESPResSo license agreement which you accepted upon receiving the distribution
// and by which you are legally bound while utilizing this file in any form or way.
// There is NO WARRANTY, not even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// You should have received a copy of that license along with this program;
// if not, refer to http://www.espresso.mpg.de/license.html where its current version can be found, or
// write to Max-Planck-Institute for Polymer Research, Theory Group, PO Box 3148, 55021 Mainz, Germany.
// Copyright (c) 2002-2004; all rights reserved unless otherwise stated.
/** \file statistics.c
    This is the place for analysation (so far...).
    Implementation of \ref statistics.h "statistics.h"
*/
#include <tcl.h>
#include <stdlib.h>
#include <string.h>
#include "statistics.h"
#include "statistics_chain.h"
#include "statistics_molecule.h"
#include "statistics_cluster.h"
#include "energy.h"
#include "modes.h"
#include "pressure.h"
#include "communication.h"
#include "debug.h"
#include "grid.h"
#include "parser.h"
#include "particle_data.h"
#include "interaction_data.h"
#include "domain_decomposition.h"
#include "verlet.h"

/** Makro definition to handle the different complex types of FFTW2 and FFTW3 */
#ifdef USEFFTW3
#  define FFTW_REAL [0]
#  define FFTW_IMAG [1]
#else
#  define FFTW_REAL .re
#  define FFTW_IMAG .im
#endif

/** Previous particle configurations (needed for offline analysis and correlation analysis in \ref #analyze) */
double **configs = NULL; int n_configs = 0; int n_part_conf = 0;

/** Variables for measuring the compressibility from volume fluctuations.
    Will be used by \ref parse_Vkappa exclusively. */
typedef struct {
  /** sum of all the considered volumes resp. volumes squared so far */
  double Vk1; double Vk2; 
  /** amount of considered volumes so far */
  double avk;
} Vkappa_struct;
Vkappa_struct Vkappa = { 0.0, 0.0, 0.0 };

/****************************************************************************************
 *                                 helper functions
 ****************************************************************************************/

double min_distance2(double pos1[3], double pos2[3])
{
  double diff[3];
  get_mi_vector(diff, pos1, pos2);
  return sqrlen(diff);
}

static int get_reference_point(Tcl_Interp *interp, int *argc, char ***argv,
			       double pos[3], int *pid)
{
  *pid = -1;
  
  if (*argc < 3) {
    Particle ref;
    if (Tcl_GetInt(interp, (*argv)[0], pid) == TCL_ERROR)
      return TCL_ERROR;
    
    if (get_particle_data(*pid, &ref) != TCL_OK) {
      Tcl_AppendResult(interp, "reference particle does not exist", (char *)NULL);
      return TCL_ERROR;
    }
    pos[0] = ref.r.p[0];
    pos[1] = ref.r.p[1];
    pos[2] = ref.r.p[2];
    
    (*argc)--;
    (*argv)++;

    free_particle(&ref);
    return TCL_OK;
  }
  /* else */
  if (Tcl_GetDouble(interp, (*argv)[0], &pos[0]) == TCL_ERROR ||
      Tcl_GetDouble(interp, (*argv)[1], &pos[1]) == TCL_ERROR ||
      Tcl_GetDouble(interp, (*argv)[2], &pos[2]) == TCL_ERROR)
    return TCL_ERROR;

  (*argc) -= 3;
  (*argv) += 3;

  return TCL_OK;
}

/****************************************************************************************
 *                                 basic observables calculation
 ****************************************************************************************/

double mindist(IntList *set1, IntList *set2)
{
  double mindist, pt[3];
  int i, j, in_set;

  mindist = SQR(box_l[0] + box_l[1] + box_l[2]);

  updatePartCfg(WITHOUT_BONDS);
  for (j=0; j<n_total_particles-1; j++) {
    pt[0] = partCfg[j].r.p[0];
    pt[1] = partCfg[j].r.p[1];
    pt[2] = partCfg[j].r.p[2];
    /* check which sets particle j belongs to
       bit 0: set1, bit1: set2
    */
    in_set = 0;
    if (!set1 || intlist_contains(set1, partCfg[j].p.type))
      in_set = 1;
    if (!set2 || intlist_contains(set2, partCfg[j].p.type))
      in_set |= 2;
    if (in_set == 0)
      continue;

    for (i=j+1; i<n_total_particles; i++)
      /* accept a pair if particle j is in set1 and particle i in set2 or vice versa. */
      if (((in_set & 1) && (!set2 || intlist_contains(set2, partCfg[i].p.type))) ||
	  ((in_set & 2) && (!set1 || intlist_contains(set1, partCfg[i].p.type))))
	mindist = dmin(mindist, min_distance2(pt, partCfg[i].r.p));
  }
  mindist = sqrt(mindist);
  return mindist;
}

int aggregation(double dist_criteria2, int min_contact, int s_mol_id, int f_mol_id, int *head_list, int *link_list, int *agg_id_list, int *agg_num, int *agg_size, int *agg_max, int *agg_min, int *agg_avg, int *agg_std)
{
  int c, np, n, i;
  Particle *p1, *p2, **pairs;
  double dist2, vec21[3];
  int target1, target2, head_p1;
  int p1molid, p2molid;
  int *contact_num, ind;

  contact_num = (int *) malloc(n_molecules*n_molecules *sizeof(int));
  for (i = 0; i < n_molecules *n_molecules; i++) contact_num[i]=0;


  build_verlet_lists();

  for (i = s_mol_id; i <= f_mol_id; i++) {
    head_list[i]=i;
    link_list[i]=-1;
    agg_id_list[i]=i;
    agg_size[i]=0;
  }
  
  /* Loop local cells */
  for (c = 0; c < local_cells.n; c++) {
    /* Loop cell neighbors */
    for (n = 0; n < dd.cell_inter[c].n_neighbors; n++) {
      pairs = dd.cell_inter[c].nList[n].vList.pair;
      np    = dd.cell_inter[c].nList[n].vList.n;
      /* verlet list loop */
      for(i=0; i<2*np; i+=2) {
	p1 = pairs[i];                    /* pointer to particle 1 */
	p2 = pairs[i+1];                  /* pointer to particle 2 */
	p1molid = p1->p.mol_id;
	p2molid = p2->p.mol_id;
	if (((p1molid <= f_mol_id) && (p1molid >= s_mol_id)) && ((p2molid <= f_mol_id) && (p2molid >= s_mol_id))) {
	  if (agg_id_list[p1molid] != agg_id_list[p2molid]) {
	    dist2 = distance2vec(p1->r.p, p2->r.p, vec21);
	    if ( p1molid > p2molid ) { ind=p1molid*n_molecules + p2molid;} 
	    else { ind=p2molid*n_molecules +p1molid;}

	    if (dist2 < dist_criteria2) {
		contact_num[ind] ++;
	    }
	    if (contact_num[ind] >= min_contact) {
	      /* merge list containing p2molid into list containing p1molid*/
	      target1=head_list[agg_id_list[p2molid]];
	      head_list[agg_id_list[p2molid]]=-2;
	      head_p1=head_list[agg_id_list[p1molid]];
	      head_list[agg_id_list[p1molid]]=target1;
	      agg_id_list[target1]=agg_id_list[p1molid];
	      target2=link_list[target1];
	      while(target2 != -1) {
		target1=target2;
		target2=link_list[target1];
		agg_id_list[target1]=agg_id_list[p1molid];
	      }
	      agg_id_list[target1]=agg_id_list[p1molid];
	      link_list[target1]=head_p1;
	    }
	  }
	}
      }
    }
  }
  
  /* count number of aggregates 
     find aggregate size
     find max and find min size, and std */
  for (i = s_mol_id ; i <= f_mol_id ; i++) {
    if (head_list[i] != -2) {
      (*agg_num)++;
      agg_size[*agg_num -1]++;
      target1= head_list[i];
      while( link_list[target1] != -1) {
	target1= link_list[target1];
	agg_size[*agg_num -1]++;
      }
    }
  }
  
  for (i = 0 ; i < *agg_num; i++) {
    *agg_avg += agg_size[i];
    *agg_std += agg_size[i] * agg_size[i];
    if (*agg_min > agg_size[i]) { *agg_min = agg_size[i]; }
    if (*agg_max < agg_size[i]) { *agg_max = agg_size[i]; }
  }
  
  return 0;
}



void centermass(int type, double *com)
{
  int i, j;
  double M = 0.0;
  com[0]=com[1]=com[2]=0.;
   	
  updatePartCfg(WITHOUT_BONDS);
  for (j=0; j<n_total_particles; j++) {
    if (type == partCfg[j].p.type) {
      for (i=0; i<3; i++) {
      	com[i] += partCfg[j].r.p[i]*PMASS(partCfg[j]);
      }
      M += PMASS(partCfg[j]);
    }
  }
  
  for (i=0; i<3; i++) {
    com[i] /= M;
  }
  return;
}


void  momentofinertiamatrix(int type, double *MofImatrix)
{
  int i,j,count;
  double p1[3],com[3],massi;

  count=0;
  updatePartCfg(WITHOUT_BONDS);
  for(i=0;i<9;i++) MofImatrix[i]=0.;
  centermass(type, com);
  for (j=0; j<n_total_particles; j++) {
    if (type == partCfg[j].p.type) {
      count ++;
      for (i=0; i<3; i++) {
      	p1[i] = partCfg[j].r.p[i] - com[i];
      }
      massi= PMASS(partCfg[j]);
      MofImatrix[0] += massi * (p1[1] * p1[1] + p1[2] * p1[2]) ; 
      MofImatrix[4] += massi * (p1[0] * p1[0] + p1[2] * p1[2]);
      MofImatrix[8] += massi * (p1[0] * p1[0] + p1[1] * p1[1]);
      MofImatrix[1] -= massi * (p1[0] * p1[1]);
      MofImatrix[2] -= massi * (p1[0] * p1[2]); 
      MofImatrix[5] -= massi * (p1[1] * p1[2]);
    }
  }
  /* use symmetry */
  MofImatrix[3] = MofImatrix[1]; 
  MofImatrix[6] = MofImatrix[2]; 
  MofImatrix[7] = MofImatrix[5];
  return;
}

void nbhood(double pt[3], double r, IntList *il)
{
  double d[3];
  int i;

  init_intlist(il);
 
  updatePartCfg(WITHOUT_BONDS);

  for (i = 0; i<n_total_particles; i++) {
    get_mi_vector(d, pt, partCfg[i].r.p);
    if (sqrt(sqrlen(d)) < r) {
      realloc_intlist(il, il->n + 1);
      il->e[il->n] = partCfg[i].p.identity;
      il->n++;
    }
  }
}

double distto(double p[3], int pid)
{
  int i;
  double d[3];
  double mindist;

  /* larger than possible */
  mindist=SQR(box_l[0] + box_l[1] + box_l[2]);
  for (i=0; i<n_total_particles; i++) {
    if (pid != partCfg[i].p.identity) {
      get_mi_vector(d, p, partCfg[i].r.p);
      mindist = dmin(mindist, sqrlen(d));
    }
  }
  return sqrt(mindist);
}

void calc_cell_gpb(double xi_m, double Rc, double ro, double gacc, int maxtry, double *result) {
  double LOG,xi_min, RM, gamma,g1,g2,gmid=0,dg,ig, f,fmid, rtb;
  int i;
  LOG    = log(Rc/ro);
  xi_min = LOG/(1+LOG);
  if(maxtry < 1) maxtry = 1;

  /* determine which of the regimes we are in: */
  if(xi_m > 1) {
    ig = 1.0;
    g1 = PI / LOG;
    g2 = PI / (LOG + xi_m/(xi_m-1.0)); }
  else if(xi_m == 1) {
    ig = 1.0;
    g1 = (PI/2.0) / LOG;
    g2 = (PI/2.0) / (LOG + 1.0); }
  else if(xi_m == xi_min) {
    ig = 1.0;
    g1 = g2 = 0.0; }
  else if(xi_m > xi_min) {
    ig = 1.0;
    g1 = (PI/2.0) / LOG;
    g2 = sqrt(3.0*(LOG-xi_m/(1.0-xi_m))/(1-pow((1.0-xi_m),-3.0))); }
  else if(xi_m > 0.0) {
    ig = -1.0;
    g1 = 1-xi_m;
    g2 = xi_m*(6.0-(3.0-xi_m)*xi_m)/(3.0*LOG); }
  else if(xi_m == 0.0) {
    ig = -1.0;
    g1 = g2 = 1-xi_m; }
  else {
    result[2]=-5.0; return;
  }

  /* decide which method to use (if any): */
  if(xi_m == xi_min) {
    gamma = 0.0;
    RM    = 0.0; }
  else if(xi_m == 0.0) {
    gamma = 1-xi_m;
    RM    = -1.0; }
  else if(ig == 1.0) {
    /* determine gamma via a bisection-search: */
    f    = atan(1.0/g1) + atan( (xi_m-1.0) / g1 ) - g1 * LOG;
    fmid = atan(1.0/g2) + atan( (xi_m-1.0) / g2 ) - g2 * LOG;
    if (f*fmid >= 0.0) {
      /* failed to bracket function value with intial guess - abort: */
      result[0]=f; result[1]=fmid; result[2]=-3.0; return;  }

    /* orient search such that the positive part of the function lies to the right of the zero */
    rtb = f < 0.0 ? (dg=g2-g1,g1) : (dg=g1-g2,g2);
    for (i = 1; i <= maxtry; i++) {
      gmid = rtb + (dg *= 0.5);
      fmid = atan(1.0/gmid) + atan((xi_m-1.0)/gmid) - gmid*LOG;
      if (fmid <= 0.0) rtb = gmid;
      if (fabs(dg) < gacc || fmid == 0.0) break;
    }

    if (fabs(dg) > gacc) {
      /* too many iterations without success - abort: */
      result[0]=gmid; result[1]=dg; result[2]=-2.0; return;  }

    /* So, these are the values for gamma and Manning-radius: */
    gamma = gmid;
    RM    = Rc * exp( -(1.0/gamma) * atan(1.0/gamma) ); }
  else if(ig == -1.0) {
    /* determine -i*gamma: */
    f = -1.0*(atanh(g2) + atanh(g2/(xi_m-1))) - g2*LOG;

    /* modified orient search, this time starting from the upper bound backwards: */
    if (f < 0.0) {
      rtb = g1;  dg = g1-g2; }
    else {
      fprintf(stderr,"WARNING: Lower boundary is actually larger than l.h.s, flipping!\n");
      rtb = g1;  dg = g1;    }
    for (i = 1; i <= maxtry; i++) {
      gmid = rtb - (dg *= 0.5);
      fmid = -1.0*(atanh(gmid) + atanh(gmid/(xi_m-1))) - gmid*LOG;
      if (fmid >= 0.0) rtb = gmid;
      if (fabs(dg) < gacc || fmid == 0.0) break;
    }
    
    if (fabs(dg) > gacc) {
      /* too many iterations without success - abort: */
      result[0]=gmid; result[1]=dg; result[2]=-2.0; return;  }

    /* So, these are the values for -i*gamma and Manning-radius: */
    gamma = gmid;
    RM    = Rc * exp( atan(1.0/gamma)/gamma ); }
  else {
    result[2]=-5.0; return;
  }

  result[0]=gamma;
  result[1]=RM;
  result[2]=ig;
  return;
}

void calc_part_distribution(int *p1_types, int n_p1, int *p2_types, int n_p2, 
			    double r_min, double r_max, int r_bins, int log_flag, 
			    double *low, double *dist)
{
  int i,j,t1,t2,ind,cnt=0;
  double inv_bin_width=0.0;
  double min_dist,min_dist2=0.0,start_dist2,act_dist2;

  start_dist2 = SQR(box_l[0] + box_l[1] + box_l[2]);
  /* bin preparation */
  *low = 0.0;
  for(i=0;i<r_bins;i++) dist[i] = 0.0;
  if(log_flag == 1) inv_bin_width = (double)r_bins/(log(r_max)-log(r_min));
  else              inv_bin_width = (double)r_bins / (r_max-r_min);

  /* particle loop: p1_types*/
  for(i=0; i<n_total_particles; i++) {
    for(t1=0; t1<n_p1; t1++) {
      if(partCfg[i].p.type == p1_types[t1]) {
	min_dist2 = start_dist2;
	/* particle loop: p2_types*/
	for(j=0; j<n_total_particles; j++) {
	  if(j != i) {
	    for(t2=0; t2<n_p2; t2++) {
	      if(partCfg[j].p.type == p2_types[t2]) {
		act_dist2 =  min_distance2(partCfg[i].r.p, partCfg[j].r.p);
		if(act_dist2 < min_dist2) { min_dist2 = act_dist2; }
	      }
	    }
	  }
	}
	min_dist = sqrt(min_dist2);
	if(min_dist <= r_max) {
	  if(min_dist >= r_min) {
	    /* calculate bin index */
	    if(log_flag == 1) ind = (int) ((log(min_dist) - log(r_min))*inv_bin_width);
	    else              ind = (int) ((min_dist - r_min)*inv_bin_width);
	    if(ind >= 0 && ind < r_bins) {
	      dist[ind] += 1.0;
	    }
	  }
	  else {
	    *low += 1.0;
	  }
	}
	cnt++;    
      }
    }
  }
  
  /* normalization */
  *low /= (double)cnt;
  for(i=0;i<r_bins;i++) dist[i] /= (double)cnt;
}

void calc_rdf(int *p1_types, int n_p1, int *p2_types, int n_p2, 
	      double r_min, double r_max, int r_bins, double *rdf)
{
  int i,j,t1,t2,ind,cnt=0;
  int mixed_flag=0,start;
  double inv_bin_width=0.0,bin_width=0.0, dist;
  double volume, bin_volume, r_in, r_out;

  if(n_p1 == n_p2) {
    for(i=0;i<n_p1;i++) 
      if( p1_types[i] != p2_types[i] ) mixed_flag=1;
  }
  else mixed_flag=1;

  bin_width     = (r_max-r_min) / (double)r_bins;
  inv_bin_width = 1.0 / bin_width;
  for(i=0;i<r_bins;i++) rdf[i] = 0.0;
  /* particle loop: p1_types*/
  for(i=0; i<n_total_particles; i++) {
    for(t1=0; t1<n_p1; t1++) {
      if(partCfg[i].p.type == p1_types[t1]) {
	/* distinguish mixed and identical rdf's */
	if(mixed_flag == 1) start = 0;
	else                start = (i+1);
	/* particle loop: p2_types*/
	for(j=start; j<n_total_particles; j++) {
	  for(t2=0; t2<n_p2; t2++) {
	    if(partCfg[j].p.type == p2_types[t2]) {
	      dist = min_distance(partCfg[i].r.p, partCfg[j].r.p);
	      if(dist > r_min && dist < r_max) {
		ind = (int) ( (dist - r_min)*inv_bin_width );
		rdf[ind]++;
	      }
	      cnt++;
	    }
	  }
	}
      }
    }
  }

  /* normalization */
  volume = box_l[0]*box_l[1]*box_l[2];
  for(i=0; i<r_bins; i++) {
    r_in       = i*bin_width + r_min; 
    r_out      = r_in + bin_width;
    bin_volume = (4.0/3.0) * PI * ((r_out*r_out*r_out) - (r_in*r_in*r_in));
    rdf[i] *= volume / (bin_volume * cnt);
  }
}

void calc_rdf_av(int *p1_types, int n_p1, int *p2_types, int n_p2,
		 double r_min, double r_max, int r_bins, double *rdf, int n_conf)
{
  int i,j,k,l,t1,t2,ind,cnt=0,cnt_conf=1;
  int mixed_flag=0,start;
  double inv_bin_width=0.0,bin_width=0.0, dist;
  double volume, bin_volume, r_in, r_out;
  double *rdf_tmp, p1[3],p2[3];

  rdf_tmp = malloc(r_bins*sizeof(double));

  if(n_p1 == n_p2) {
    for(i=0;i<n_p1;i++)
      if( p1_types[i] != p2_types[i] ) mixed_flag=1;
  }
  else mixed_flag=1;
  bin_width     = (r_max-r_min) / (double)r_bins;
  inv_bin_width = 1.0 / bin_width;
  volume = box_l[0]*box_l[1]*box_l[2];
  for(l=0;l<r_bins;l++) rdf_tmp[l]=rdf[l] = 0.0;

  while(cnt_conf<=n_conf) {
    for(l=0;l<r_bins;l++) rdf_tmp[l]=0.0;
    cnt=0;
    k=n_configs-cnt_conf;
    for(i=0; i<n_total_particles; i++) {
      for(t1=0; t1<n_p1; t1++) {
	if(partCfg[i].p.type == p1_types[t1]) {
	  // distinguish mixed and identical rdf's
	  if(mixed_flag == 1) start = 0;
	  else                start = (i+1);
	  //particle loop: p2_types
	  for(j=start; j<n_total_particles; j++) {
	    for(t2=0; t2<n_p2; t2++) {
	      if(partCfg[j].p.type == p2_types[t2]) {
		p1[0]=configs[k][3*i  ];p1[1]=configs[k][3*i+1];p1[2]=configs[k][3*i+2];
		p2[0]=configs[k][3*j  ];p2[1]=configs[k][3*j+1];p2[2]=configs[k][3*j+2];
		dist =min_distance(p1, p2);
		if(dist > r_min && dist < r_max) {
		  ind = (int) ( (dist - r_min)*inv_bin_width );
		  rdf_tmp[ind]++;
		}
		cnt++;
	      }
	    }
	  }
	}
      }
    }
    // normalization
  
    for(i=0; i<r_bins; i++) {
      r_in       = i*bin_width + r_min;
      r_out      = r_in + bin_width;
      bin_volume = (4.0/3.0) * PI * ((r_out*r_out*r_out) - (r_in*r_in*r_in));
      rdf[i] += rdf_tmp[i]*volume / (bin_volume * cnt);
    }

    cnt_conf++;
  } //cnt_conf loop
  for(i=0; i<r_bins; i++) {
    rdf[i] /= (cnt_conf-1);
  }
  free(rdf_tmp);

}

void calc_rdf_intermol_av(int *p1_types, int n_p1, int *p2_types, int n_p2,
			  double r_min, double r_max, int r_bins, double *rdf, int n_conf)
{
  int i,j,k,l,t1,t2,ind,cnt=0,cnt_conf=1;
  int mixed_flag=0,start;
  double inv_bin_width=0.0,bin_width=0.0, dist;
  double volume, bin_volume, r_in, r_out;
  double *rdf_tmp, p1[3],p2[3];

  rdf_tmp = malloc(r_bins*sizeof(double));

  if(n_p1 == n_p2) {
    for(i=0;i<n_p1;i++)
      if( p1_types[i] != p2_types[i] ) mixed_flag=1;
  }
  else mixed_flag=1;
  bin_width     = (r_max-r_min) / (double)r_bins;
  inv_bin_width = 1.0 / bin_width;
  volume = box_l[0]*box_l[1]*box_l[2];
  for(l=0;l<r_bins;l++) rdf_tmp[l]=rdf[l] = 0.0;

  while(cnt_conf<=n_conf) {
    for(l=0;l<r_bins;l++) rdf_tmp[l]=0.0;
    cnt=0;
    k=n_configs-cnt_conf;
    for(i=0; i<n_total_particles; i++) {
      for(t1=0; t1<n_p1; t1++) {
	if(partCfg[i].p.type == p1_types[t1]) {
	  // distinguish mixed and identical rdf's
	  if(mixed_flag == 1) start = 0;
	  else                start = (i+1);
	  //particle loop: p2_types
	  for(j=start; j<n_total_particles; j++) {
	    for(t2=0; t2<n_p2; t2++) {
	      if(partCfg[j].p.type == p2_types[t2]) {
		/*see if particles i and j belong to different molecules*/
		if(partCfg[i].p.mol_id!=partCfg[j].p.mol_id) {
		  p1[0]=configs[k][3*i  ];p1[1]=configs[k][3*i+1];p1[2]=configs[k][3*i+2];
		  p2[0]=configs[k][3*j  ];p2[1]=configs[k][3*j+1];p2[2]=configs[k][3*j+2];
		  dist =min_distance(p1, p2);
		  if(dist > r_min && dist < r_max) {
		    ind = (int) ( (dist - r_min)*inv_bin_width );
		    rdf_tmp[ind]++;
		  }
		  cnt++;
		}
	      }
	    }
	  }
	}
      }
    }
    // normalization

    for(i=0; i<r_bins; i++) {
      r_in       = i*bin_width + r_min;
      r_out      = r_in + bin_width;
      bin_volume = (4.0/3.0) * PI * ((r_out*r_out*r_out) - (r_in*r_in*r_in));
      rdf[i] += rdf_tmp[i]*volume / (bin_volume * cnt);
    }

    cnt_conf++;
  } //cnt_conf loop
  for(i=0; i<r_bins; i++) {
    rdf[i] /= (cnt_conf-1);
  }
  free(rdf_tmp);

}

void analyze_structurefactor(int type, int order, double **_ff) {
  int i, j, k, n, qi, p, order2, *fn=NULL;
  double qr, twoPI_L, C_sum, S_sum, *ff=NULL;
  
  order2 = order*order;
  *_ff = ff = realloc(ff,(order2+1)*sizeof(double));
  fn = realloc(fn,(order2+1)*sizeof(int));
  twoPI_L = 2*PI/box_l[0];
  
  if ((type < 0) || (type > n_particle_types)) { fprintf(stderr,"WARNING: Type %i does not exist!",type); fflush(NULL); errexit(); }
  else if (order < 1) { fprintf(stderr,"WARNING: parameter \"order\" has to be a whole positive number"); fflush(NULL); errexit(); }
  else {
    for(qi=1; qi<=order2; qi++) {
      ff[qi] = 0.0;
      fn[qi] = 0;
    }
    for(i=0; i<=order; i++) {
      for(j=-order; j<=order; j++) {
        for(k=-order; k<=order; k++) {
	  n = i*i + j*j + k*k;
	  if ((n<=order2) && (n>0)) {
	    C_sum = S_sum = 0.0;
	    for(p=0; p<n_total_particles; p++) {
	      if (partCfg[p].p.type == type) {
		qr = twoPI_L * ( i*partCfg[p].r.p[0] + j*partCfg[p].r.p[1] + k*partCfg[p].r.p[2] );
		C_sum+= cos(qr);
		S_sum+= sin(qr);
	      }
	    }
	    ff[n]+= C_sum*C_sum + S_sum*S_sum;
	    fn[n]++;
	  }
	}
      }
    }
    n = 0;
    for(p=0; p<n_total_particles; p++) {
      if (partCfg[p].p.type == type) n++;
    }
    for(qi=0; qi<=order2; qi++) 
      if (fn[qi]!=0) ff[qi]/= n*fn[qi];
    free(fn);
  }
}

/****************************************************************************************
 *                                 config storage functions
 ****************************************************************************************/

void analyze_append() {
  int i;
  n_part_conf = n_total_particles;
  configs = realloc(configs,(n_configs+1)*sizeof(double *));
  configs[n_configs] = (double *) malloc(3*n_part_conf*sizeof(double));
  for(i=0; i<n_part_conf; i++) {
    configs[n_configs][3*i]   = partCfg[i].r.p[0];
    configs[n_configs][3*i+1] = partCfg[i].r.p[1];
    configs[n_configs][3*i+2] = partCfg[i].r.p[2];
  }
  n_configs++;
}

void analyze_push() {
  int i;
  n_part_conf = n_total_particles;
  free(configs[0]);
  for(i=0; i<n_configs-1; i++) {
    configs[i]=configs[i+1];
  }
  configs[n_configs-1] = (double *) malloc(3*n_part_conf*sizeof(double));
  for(i=0; i<n_part_conf; i++) {
    configs[n_configs-1][3*i]   = partCfg[i].r.p[0];
    configs[n_configs-1][3*i+1] = partCfg[i].r.p[1];
    configs[n_configs-1][3*i+2] = partCfg[i].r.p[2];
  }
}

void analyze_replace(int ind) {
  int i;
  n_part_conf = n_total_particles;
  for(i=0; i<n_part_conf; i++) {
    configs[ind][3*i]   = partCfg[i].r.p[0];
    configs[ind][3*i+1] = partCfg[i].r.p[1];
    configs[ind][3*i+2] = partCfg[i].r.p[2];
  }
}

void analyze_remove(int ind) {
  int i;
  free(configs[ind]);
  for(i=ind; i<n_configs-1; i++) {
    configs[i]=configs[i+1];
  }
  n_configs--;
  configs = realloc(configs,n_configs*sizeof(double *));
  if (n_configs == 0) n_part_conf = 0;
}

void analyze_configs(double *tmp_config, int count) {
  int i;
  n_part_conf = count;
  configs = realloc(configs,(n_configs+1)*sizeof(double *));
  configs[n_configs] = (double *) malloc(3*n_part_conf*sizeof(double));
  for(i=0; i<n_part_conf; i++) {
    configs[n_configs][3*i]   = tmp_config[3*i];
    configs[n_configs][3*i+1] = tmp_config[3*i+1];
    configs[n_configs][3*i+2] = tmp_config[3*i+2];
  }
  n_configs++;
}

void analyze_activate(int ind) {
  int i;
  double pos[3];
  n_part_conf = n_total_particles;

  for(i=0; i<n_part_conf; i++) {
    pos[0] = configs[ind][3*i];
    pos[1] = configs[ind][3*i+1];
    pos[2] = configs[ind][3*i+2];
    if (place_particle(i, pos)==TCL_ERROR) {
      char *errtxt = runtime_error(128 + TCL_INTEGER_SPACE);
      ERROR_SPRINTF(errtxt, "{057 failed upon replacing particle %d in Espresso} ", i); 
    }
  }
}


/****************************************************************************************
 *                                 Observables handling
 ****************************************************************************************/

void obsstat_realloc_and_clear(Observable_stat *stat, int n_pre, int n_bonded, int n_non_bonded,
			       int n_coulomb, int c_size)
{
  int i, total = c_size*(n_pre + n_bonded_ia + n_non_bonded + n_coulomb);

  realloc_doublelist(&(stat->data), stat->data.n = total);
  stat->chunk_size = c_size;
  stat->n_coulomb    = n_coulomb;
  stat->n_non_bonded = n_non_bonded;
  stat->bonded     = stat->data.e + c_size*n_pre;
  stat->non_bonded = stat->bonded + c_size*n_bonded_ia;
  stat->coulomb    = stat->non_bonded + c_size*n_non_bonded;

  for(i = 0; i < total; i++)
    stat->data.e[i] = 0.0;
}

void invalidate_obs()
{ 
  total_energy.init_status = 0;
  total_pressure.init_status = 0;
}



/****************************************************************************************
 *                                 basic observables parsing
 ****************************************************************************************/

static int parse_get_folded_positions(Tcl_Interp *interp, int argc, char **argv)
{
  char buffer[10 + 3*TCL_DOUBLE_SPACE + TCL_INTEGER_SPACE];
  int i,change ;
  double shift[3];
  float *coord;

  shift[0] = shift[1] = shift[2] = 0.0;

  enum flag { NONE , FOLD_MOLS};

  int flag;
  flag = NONE;

  change = 0;
  shift[0] = shift[1] = shift[2] = 0.0;

  STAT_TRACE(fprintf(stderr,"%d,parsing get_folded_positions \n",this_node));
  while (argc > 0)
    {
      if ( ARG0_IS_S("-molecule") ) {
	flag = FOLD_MOLS;
	change = 1;
      }

      if ( ARG0_IS_S("shift") ) {
	if ( !ARG_IS_D(1,shift[0]) || !ARG_IS_D(2,shift[1]) || !ARG_IS_D(3,shift[2]) ) {
	  Tcl_ResetResult(interp);
	  Tcl_AppendResult(interp,"usage: analyze get_folded_positions [-molecule] [shift <xshift> <yshift> <zshift>]", (char *)NULL);
	  return (TCL_ERROR);
	}
	change = 4;
      }
      argc -= change;
      argv += change;
      STAT_TRACE(fprintf(stderr,"%d,argc = %d \n",this_node, argc));
    }


  updatePartCfg(WITH_BONDS);
  if (!sortPartCfg()) {
    char *errtxt = runtime_error(128);
    ERROR_SPRINTF(errtxt, "{058 could not sort partCfg, particles have to start at 0 and have consecutive identities} ");
    return TCL_ERROR;
  }
  coord = malloc(n_total_particles*3*sizeof(float));
  /* Construct the array coord*/
  for (i = 0; i < n_total_particles; i++) {
    int dummy[3] = {0,0,0};
    double tmpCoord[3];
    tmpCoord[0] = partCfg[i].r.p[0];
    tmpCoord[1] = partCfg[i].r.p[1];
    tmpCoord[2] = partCfg[i].r.p[2];
    if (flag == NONE)  {   // perform folding by particle
      fold_position(tmpCoord, dummy);
    }    
    coord[i*3    ] = (float)(tmpCoord[0]);
    coord[i*3 + 1] = (float)(tmpCoord[1]);
    coord[i*3 + 2] = (float)(tmpCoord[2]);
  }


  // Use information from the analyse set command to fold chain molecules
  if ( flag == FOLD_MOLS ) {
    if( analyze_fold_molecules(coord, shift) != TCL_OK ){
      Tcl_AppendResult(interp, "could not fold chains: \"analyze set chains <chain_start> <n_chains> <chain_length>\" must be used first",
		       (char *) NULL);
      return (TCL_ERROR);;   
    }
  }

  //  Tcl_AppendResult(interp, "{ ", (char *)NULL);
  for ( i = 0 ; i < n_total_particles ; i++) {
    sprintf(buffer, " { %d %f %f %f } ", partCfg[i].p.identity , coord[i*3] , coord[i*3+1] , coord[i*3+2] );
    Tcl_AppendResult(interp, buffer , (char *)NULL);
  }
  //  Tcl_AppendResult(interp, "} ", (char *)NULL);

  return TCL_OK;

}


static int parse_get_lipid_orients(Tcl_Interp *interp, int argc, char **argv)
{
  char buffer[TCL_DOUBLE_SPACE + TCL_INTEGER_SPACE];
  int i,change ;
  IntList l_orient;
  init_intlist(&l_orient);

  change = 0;

  STAT_TRACE(fprintf(stderr,"%d,parsing get_lipid_orients \n",this_node));
  while (argc > 0)
    {
      if ( ARG0_IS_S("setgrid") ) { 
	if ( !ARG_IS_I(1,mode_grid_3d[0]) || !ARG_IS_I(2,mode_grid_3d[1]) || !ARG_IS_I(3,mode_grid_3d[2]) ) {
	  Tcl_ResetResult(interp);
	  Tcl_AppendResult(interp,"usage: analyze get_lipid_orients [setgrid <xdim> <ydim> <zdim>] [setstray <stray_cut_off>]", (char *)NULL);
	  return (TCL_ERROR);
	}
	STAT_TRACE(fprintf(stderr,"%d,setgrid has args %d,%d,%d \n",this_node,mode_grid_3d[0],mode_grid_3d[1],mode_grid_3d[2]));
	change = 4;
	/* Update global parameters */
	map_to_2dgrid();
	mode_grid_changed = 1;
	  
      }
      if ( ARG0_IS_S("setstray") ) { 
	if ( !ARG_IS_D(1,stray_cut_off) ) {
	  Tcl_ResetResult(interp);
	  Tcl_AppendResult(interp,"usage: analyze get_lipid_orients [setgrid <xdim> <ydim> <zdim>] [setstray <stray_cut_off>]", (char *)NULL);
	  return (TCL_ERROR);
	}
	STAT_TRACE(fprintf(stderr,"%d,setgrid has args %d,%d,%d \n",this_node,mode_grid_3d[0],mode_grid_3d[1],mode_grid_3d[2]));
	change = 2;
      }
      argc -= change;
      argv += change;
      STAT_TRACE(fprintf(stderr,"%d,argc = %d \n",this_node, argc));
      
      
    }
  
  realloc_intlist(&l_orient, n_molecules);
  get_lipid_orients(&l_orient);
  

  Tcl_AppendResult(interp, "{ Lipid_orientations } { ", (char *)NULL);
  for ( i = 0 ; i < n_molecules ; i++) {
    sprintf(buffer, "%d ", l_orient.e[i]);
    Tcl_AppendResult(interp, buffer , (char *)NULL);
  }
  Tcl_AppendResult(interp, "} ", (char *)NULL);

  realloc_intlist(&l_orient,0);

  return TCL_OK;

}

static int parse_modes2d(Tcl_Interp *interp, int argc, char **argv)
{
  STAT_TRACE(fprintf(stderr,"%d,parsing modes2d \n",this_node);)
    /* 'analyze modes2d [setgrid <xdim> <ydim> <zdim>] [setstray <stray_cut_off>]]' */
    char buffer[TCL_DOUBLE_SPACE];
  int i,j,change ;
  fftw_complex* result;

  change = 0;


  if (n_total_particles <= 2) {
    Tcl_AppendResult(interp, "(not enough particles for mode analysis)",
		     (char *)NULL);
    return (TCL_OK);
  }

  while (argc > 0)
    {
      if ( ARG0_IS_S("setgrid") ) { 
	if ( !ARG_IS_I(1,mode_grid_3d[0]) || !ARG_IS_I(2,mode_grid_3d[1]) || !ARG_IS_I(3,mode_grid_3d[2]) ) {
	  Tcl_ResetResult(interp);
	  Tcl_AppendResult(interp,"usage: analyze modes2d [setgrid <xdim> <ydim> <zdim>] [setstray <stray_cut_off>]", (char *)NULL);
	  return (TCL_ERROR);
	}
	STAT_TRACE(fprintf(stderr,"%d,setgrid has args %d,%d,%d \n",this_node,mode_grid_3d[0],mode_grid_3d[1],mode_grid_3d[2]));
	change = 4;
	/* Update global parameters */
	map_to_2dgrid();
	mode_grid_changed = 1;
	
      }
      if ( ARG0_IS_S("setstray") ) { 
	if ( !ARG_IS_D(1,stray_cut_off) ) {
	  Tcl_ResetResult(interp);
	  Tcl_AppendResult(interp,"usage: analyze modes2d [setgrid <xdim> <ydim> <zdim>] [setstray <stray_cut_off>]", (char *)NULL);
	  return (TCL_ERROR);
	}
	STAT_TRACE(fprintf(stderr,"%d,setgrid has args %d,%d,%d \n",this_node,mode_grid_3d[0],mode_grid_3d[1],mode_grid_3d[2]));
	change = 2;
      }
      argc -= change;
      argv += change;
      STAT_TRACE(fprintf(stderr,"%d,argc = %d \n",this_node, argc);)

	
	}


  result = malloc((mode_grid_3d[ydir]/2+1)*(mode_grid_3d[xdir])*sizeof(fftw_complex));

  if (!modes2d(result)) {
    fprintf(stderr,"%d,mode analysis failed \n",this_node);
    return TCL_ERROR;
  }
  else {    STAT_TRACE(fprintf(stderr,"%d,mode analysis done \n",this_node));}
  

  Tcl_AppendResult(interp, "{ Modes } { ", (char *)NULL);
  for ( i = 0 ; i < mode_grid_3d[xdir] ; i++) {
    Tcl_AppendResult(interp, " { ", (char *)NULL);
    for ( j = 0 ; j < mode_grid_3d[ydir]/2 + 1 ; j++) {
      Tcl_AppendResult(interp, " { ", (char *)NULL);
      Tcl_PrintDouble(interp,result[j+i*(mode_grid_3d[ydir]/2+1)]FFTW_REAL,buffer);
      Tcl_AppendResult(interp, buffer, (char *)NULL);
      Tcl_AppendResult(interp, " ", (char *)NULL);
      Tcl_PrintDouble(interp,result[j+i*(mode_grid_3d[ydir]/2+1)]FFTW_IMAG,buffer);
      Tcl_AppendResult(interp, buffer, (char *)NULL);
      Tcl_AppendResult(interp, " } ", (char *)NULL);
    }
    Tcl_AppendResult(interp, " } ", (char *)NULL);
  }


  Tcl_AppendResult(interp, " } ", (char *)NULL);

  free(result);

  return TCL_OK;

}

static int parse_lipid_orient_order(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze lipid_orient_order ' */
  double result;
  char buffer[TCL_DOUBLE_SPACE];
  result = 0;

  if (n_total_particles <= 1) {
    Tcl_AppendResult(interp, "(not enough particles)",
		     (char *)NULL);
    return (TCL_OK);
  }

  if ( orient_order(&result) == TCL_OK ) {
    Tcl_PrintDouble(interp, result, buffer);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
    return TCL_OK;
  }

  Tcl_AppendResult(interp, "Error calculating orientational order ", (char *)NULL);
  return TCL_ERROR;
}



static int parse_aggregation(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze centermass [<type>]' */
  char buffer[256 + 3*TCL_INTEGER_SPACE + 2*TCL_DOUBLE_SPACE];
  int i, target1;
  int *agg_id_list;
  double dist_criteria, dist_criteria2;
  int min_contact, *head_list, *link_list;
  int agg_num =0, *agg_size, agg_min= n_molecules, agg_max = 0,  agg_std = 0, agg_avg = 0; 
  float fagg_avg;
  int s_mol_id, f_mol_id;

  agg_id_list = (int *) malloc(n_molecules *sizeof(int));
  head_list =  (int *) malloc(n_molecules *sizeof(int));
  link_list = (int *) malloc(n_molecules *sizeof(int));
  agg_size = (int *) malloc(n_molecules *sizeof(int));

  /* parse arguments */
  if (argc < 3) {
    Tcl_AppendResult(interp, "usage: analyze aggregation <dist_criteria> <start mol_id> <finish mol_id> [<min_contact>]", (char *)NULL);
    return (TCL_ERROR);
  }

  if (!ARG_IS_D(0,dist_criteria)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage:  analyze aggregation  <dist_criteria> <start mol_id> <finish mol_id> [<min_contact>]", (char *)NULL);
    return (TCL_ERROR);
  }
  dist_criteria2 = dist_criteria * dist_criteria;

  if (!ARG_IS_I(1,s_mol_id)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage:  analyze aggregation  <dist_criteria> <start mol_id> <finish mol_id> [<min_contact>]", (char *)NULL);
    return (TCL_ERROR);
  }
  if (!ARG_IS_I(2,f_mol_id)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage:  analyze aggregation  <dist_criteria> <start mol_id> <finish mol_id> [<min_contact>]", (char *)NULL);
    return (TCL_ERROR);
  }
  
  if (n_nodes > 1) {
    Tcl_AppendResult(interp, "aggregation can only be calculated on a single processor", (char *)NULL);
    return TCL_ERROR;
  }

  if (cell_structure.type != CELL_STRUCTURE_DOMDEC) {
    Tcl_AppendResult(interp, "aggregation can only be calculated with the domain decomposition cell system", (char *)NULL);
    return TCL_ERROR;
  }

  if ( (s_mol_id < 0) || (s_mol_id > n_molecules) || (f_mol_id < 0) || (f_mol_id > n_molecules) ) {
    Tcl_AppendResult(interp, "check your start and finish molecule id's", (char *)NULL);
    return TCL_ERROR;
  }

  if ( max_range_non_bonded2 < dist_criteria2) {
    Tcl_AppendResult(interp, "dist_criteria is larger than max_range_non_bonded.", (char *)NULL);
    return TCL_ERROR;    
    
  }

  if (argc == 4) {
      if (!ARG_IS_I(3,min_contact)) {
	  Tcl_ResetResult(interp);
	  Tcl_AppendResult(interp, "usage: analyze aggregation <dist_criteria> <start mol_id> <finish mol_id> [<min_contact>]", (char *)NULL);
	  return (TCL_ERROR);
      }
  } else {
      min_contact = 1;
  }


  aggregation(dist_criteria2, min_contact, s_mol_id, f_mol_id, head_list, link_list, agg_id_list, 
	      &agg_num, agg_size, &agg_max, &agg_min, &agg_avg, &agg_std);

  fagg_avg = (float) (agg_avg)/agg_num;
  sprintf (buffer, " MAX %d MIN %d AVG %f STD %f AGG_NUM %d AGGREGATES", 
	   agg_max, agg_min, fagg_avg, sqrt( (float) (agg_std/(float)(agg_num)-fagg_avg*fagg_avg)), agg_num);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  
  for (i = s_mol_id ; i <= f_mol_id ; i++) {
    if (head_list[i] != -2) {
      target1= head_list[i];
      sprintf(buffer, " { %d ", target1); 
      Tcl_AppendResult(interp, buffer, (char *)NULL);
      while( link_list[target1] != -1) {
	target1= link_list[target1];
	sprintf(buffer, "%d ", target1); 
	Tcl_AppendResult(interp, buffer, (char *)NULL); 
      }
      sprintf(buffer, "} ");
      Tcl_AppendResult(interp, buffer, (char *)NULL);
    }
  }

  free(agg_id_list);
  free(head_list);
  free(link_list);
  free(agg_size);

  return TCL_OK;
}


static int parse_mindist(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze mindist [<type_list_a> <type_list_b>]' */
  double result;
  char buffer[TCL_DOUBLE_SPACE];
  IntList p1, p2;

  init_intlist(&p1); init_intlist(&p2);

  if (n_total_particles <= 1) {
    Tcl_AppendResult(interp, "(not enough particles)",
		     (char *)NULL);
    return (TCL_OK);
  }
  if (argc == 0)
    result = mindist(NULL, NULL);
  else {
    /* parse arguments */
    if (argc < 2) {
      Tcl_AppendResult(interp, "usage: analyze mindist [<type_list> <type_list>]", (char *)NULL);
      return (TCL_ERROR);
    }

    if (!ARG0_IS_INTLIST(p1)) {
      Tcl_ResetResult(interp);
      Tcl_AppendResult(interp, "usage: analyze mindist [<type_list> <type_list>]", (char *)NULL);
      return (TCL_ERROR);
    }
    if (!ARG1_IS_INTLIST(p2)) {
      Tcl_ResetResult(interp);
      Tcl_AppendResult(interp, "usage: analyze mindist [<type_list> <type_list>]", (char *)NULL);
      return (TCL_ERROR);
    }
    result = mindist(&p1, &p2);
  }

  Tcl_PrintDouble(interp, result, buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  return TCL_OK;
}
static int parse_centermass(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze centermass [<type>]' */
  double com[3];
  char buffer[3*TCL_DOUBLE_SPACE+3];
  int p1;
  
  /* parse arguments */
  if (argc != 1) {
    Tcl_AppendResult(interp, "usage: analyze centermass [<type>]", (char *)NULL);
    return (TCL_ERROR);
  }

  if (!ARG0_IS_I(p1)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze centermass [<type>]", (char *)NULL);
    return (TCL_ERROR);
  }
  
  centermass(p1, com);
  
  sprintf(buffer,"%f %f %f",com[0],com[1],com[2]);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  return TCL_OK;
}

static int parse_momentofinertiamatrix(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze  momentofinertiamatrix [<type>]' */
  double MofImatrix[9];
  char buffer[9*TCL_DOUBLE_SPACE+9];
  int p1;

  /* parse arguments */
  if (argc != 1) {
    Tcl_AppendResult(interp, "usage: analyze momentofinertiamatrix [<type>]", (char *)NULL);
    return (TCL_ERROR);
  }

  if (!ARG0_IS_I(p1)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze momentofinertiamatrix [<type>]", (char *)NULL);
    return (TCL_ERROR);
  }
  momentofinertiamatrix(p1, MofImatrix);
  
  sprintf(buffer,"%f %f %f %f %f %f %f %f %f",
	  MofImatrix[0],MofImatrix[1],MofImatrix[2],MofImatrix[3],MofImatrix[4],
	  MofImatrix[5],MofImatrix[6],MofImatrix[7],MofImatrix[8]);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  return TCL_OK;
}

static int parse_find_principal_axis(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze find_principal_axis [<type0>]' */
  double MofImatrix[9],eva[3],eve[3];
  char buffer[4*TCL_DOUBLE_SPACE+20];
  int p1,i,j;

  /* parse arguments */
  if (argc != 1) {
    Tcl_AppendResult(interp, "usage: analyze find_principal_axis [<type>]", (char *)NULL);
    return (TCL_ERROR);
  }

  if (!ARG0_IS_I(p1)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze find_principal_axis [<type>]", (char *)NULL);
    return (TCL_ERROR);
  }

  momentofinertiamatrix(p1, MofImatrix);
  i=calc_eigenvalues_3x3(MofImatrix, eva);
  
  sprintf(buffer,"{eigenval eigenvector} ");
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  for (j= 0; j < 3; j++) {
    i=calc_eigenvector_3x3(MofImatrix,eva[j],eve);
    sprintf(buffer," { %f { %f %f %f } }",eva[j],eve[0],eve[1],eve[2]);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
  }
  return TCL_OK;
}

static int parse_nbhood(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze nbhood { <partid> | <posx> <posy> <posz> } <r_catch>' */
  int p, i;
  double pos[3];
  double r_catch;
  char buffer[TCL_INTEGER_SPACE + 2];  
  IntList il;

  if (n_total_particles == 0) {
    Tcl_AppendResult(interp, "(no particles)",
		     (char *)NULL);
    return (TCL_OK);
  }

  get_reference_point(interp, &argc, &argv, pos, &p);

  if (argc != 1) {
    Tcl_AppendResult(interp, "usage: nbhood { <partid> | <posx> <posy> <posz> } <r_catch>",
		     (char *)NULL);
    return TCL_ERROR;
  }

  if (!ARG0_IS_D(r_catch))
    return (TCL_ERROR);

  updatePartCfg(WITHOUT_BONDS);

  nbhood(pos, r_catch, &il);
  
  for (i = 0; i < il.n; i++) {
    sprintf(buffer, "%d ", il.e[i]);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
  }
  realloc_intlist(&il, 0);
  return (TCL_OK);
}

static int parse_distto(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze distto { <part_id> | <posx> <posy> <posz> }' */
  double result;
  int p;
  double pos[3];
  char buffer[TCL_DOUBLE_SPACE];  

  if (n_total_particles == 0) {
    Tcl_AppendResult(interp, "(no particles)",
		     (char *)NULL);
    return (TCL_OK);
  }

  get_reference_point(interp, &argc, &argv, pos, &p);
  if (argc != 0) {
    Tcl_AppendResult(interp, "usage: distto { <partid> | <posx> <posy> <posz> }",
		     (char *)NULL);
    return TCL_ERROR;
  }

  updatePartCfg(WITHOUT_BONDS);

  result = distto(pos, p);

  Tcl_PrintDouble(interp, result, buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  return (TCL_OK);
}


static int parse_cell_gpb(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze cell_gpb <Manning parameter> <outer cell radius> <inner cell radius> [<accuracy> [<# of interations>]]' */
  double result[3], xi_m, Rc, ro;
  double gacc = 1e-6;
  int maxtry  = 30000;
  char buffer[3*TCL_DOUBLE_SPACE+20], usage[150];
  sprintf(usage,"analyze cell_gpb <Manning parameter> <outer cell radius> <inner cell radius> [<accuracy> [<# of interations>]]");

  if ((argc < 3) || (argc > 5)) { 
    Tcl_AppendResult(interp, "usage: ",usage,(char *)NULL); return TCL_ERROR; }
  else if (!ARG_IS_D(0,xi_m) || !ARG_IS_D(1,Rc) || !ARG_IS_D(2,ro))
    return TCL_ERROR;
  if (argc == 4) if (!ARG_IS_D(3,gacc))
    return TCL_ERROR;
  if (argc == 5) if (!ARG_IS_I(4,maxtry))
    return TCL_ERROR;
  if ((xi_m < 0) || !((Rc > 0) && (ro > 0))) {
    Tcl_ResetResult(interp); sprintf(buffer,"%f %f %f",xi_m,Rc,ro);
    Tcl_AppendResult(interp, "usage: ",usage,"\n",(char *)NULL);
    Tcl_AppendResult(interp, "ERROR: All three arguments must be positive, the latter two even non-zero (got: ",buffer,")! Aborting...", (char*)NULL);
    return(TCL_ERROR);
  }

  calc_cell_gpb(xi_m,Rc,ro,gacc,maxtry,result);

  if (result[2] == -2.0) {
    Tcl_ResetResult(interp); sprintf(buffer,"%d",maxtry); Tcl_AppendResult(interp, "ERROR: Maximum number of iterations exceeded (",buffer,")! ");
    sprintf(buffer,"%f and %f",result[0],result[1]);      Tcl_AppendResult(interp, "Got ",buffer," so far, aborting now...", (char*)NULL);
    return(TCL_ERROR); 
  } else if (result[2] == -3.0) {
    Tcl_ResetResult(interp); sprintf(buffer,"%f and %f",result[0],result[1]);
    Tcl_AppendResult(interp, "ERROR: gamma is not bracketed by the programs initial guess (",buffer,")! Aborting...", (char*)NULL);
    return(TCL_ERROR); 
  } else if (result[2] == -4.0) {
    Tcl_ResetResult(interp); sprintf(buffer,"%f and %f",result[0],result[1]);
    Tcl_AppendResult(interp, "ERROR: lower boundary on wrong side of the function (",buffer,")! Aborting...", (char*)NULL);
    return(TCL_ERROR); 
  } else if (result[2] == -5.0) {
    Tcl_ResetResult(interp); Tcl_AppendResult(interp, "ERROR: Something went wrong! Aborting...", (char*)NULL);
    return(TCL_ERROR); 
  }
  sprintf(buffer,"%f %f %f",result[0],result[1],result[2]);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  return (TCL_OK);
}


static int parse_Vkappa(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze Vkappa [{ reset | read | set <Vk1> <Vk2> <avk> }]' */
  double result = 0.0;
  char buffer[3*TCL_DOUBLE_SPACE+3];

  if (argc > 0)
    if (ARG0_IS_S("reset"))
      Vkappa.Vk1 = Vkappa.Vk2 = Vkappa.avk = 0.0;
    else if (ARG0_IS_S("read")) {
      sprintf(buffer,"%f %f %f ",Vkappa.Vk1,Vkappa.Vk2,Vkappa.avk);
      Tcl_AppendResult(interp, buffer, (char *)NULL); return (TCL_OK); }
    else if (ARG0_IS_S("set")) {
      if (argc < 4 || !ARG_IS_D(1,Vkappa.Vk1) || !ARG_IS_D(2,Vkappa.Vk2) || !ARG_IS_D(3,Vkappa.avk)) {
        Tcl_AppendResult(interp, "usage: analyze Vkappa [{ reset | read | set <Vk1> <Vk2> <avk> }] ", (char *)NULL);  return TCL_ERROR;  }
      if (Vkappa.avk <= 0.0) {
        Tcl_AppendResult(interp, "ERROR: # of averages <avk> must be positiv! Resetting values...", (char *)NULL);
        result = Vkappa.Vk1 = Vkappa.Vk2 = Vkappa.avk = 0.0; return TCL_ERROR; }
      result = Vkappa.Vk2/Vkappa.avk - SQR(Vkappa.Vk1/Vkappa.avk); }
    else {
      Tcl_AppendResult(interp, "usage: analyze Vkappa [{ reset | read | set <Vk1> <Vk2> <avk> }] ", (char *)NULL);  return TCL_ERROR;  }
  else {
    Vkappa.Vk1 += box_l[0]*box_l[1]*box_l[2];
    Vkappa.Vk2 += SQR(box_l[0]*box_l[1]*box_l[2]);
    Vkappa.avk += 1.0;
    result = Vkappa.Vk2/Vkappa.avk - SQR(Vkappa.Vk1/Vkappa.avk);
  }

  Tcl_PrintDouble(interp, result, buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  return (TCL_OK);
}


static int parse_distribution(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze distribution { <part_type_list_a> } { <part_type_list_b> } [<r_min> [<r_max> [<r_bins> [<log_flag> [<int_flag>]]]]]' */
  /*********************************************************************************************************************************/
  char buffer[2*TCL_DOUBLE_SPACE+3*TCL_INTEGER_SPACE+256];
  IntList p1,p2;
  double r_min=0, r_max=-1.0;
  int r_bins=0, log_flag=0, int_flag=0;
  int i;
  double *distribution, low;

  init_intlist(&p1); init_intlist(&p2);

  if (argc < 2) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze distribution [<type_list> <type_list>]", (char *)NULL);
    return (TCL_ERROR);
  }

  if (!ARG0_IS_INTLIST(p1)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze distribution [<type_list> <type_list>]", (char *)NULL);
    return (TCL_ERROR);
  }
  if (!ARG1_IS_INTLIST(p2)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze distribution [<type_list> <type_list>]", (char *)NULL);
    return (TCL_ERROR);
  }

  argc -= 2; argv += 2;

  if( argc>0 ) { if (!ARG0_IS_D(r_min)) return (TCL_ERROR); argc--; argv++; }
  if( argc>0 ) { if (!ARG0_IS_D(r_max)) return (TCL_ERROR); argc--; argv++; }
  if( argc>0 ) { if (!ARG0_IS_I(r_bins)) return (TCL_ERROR);   argc--; argv++; }
  if( argc>0 ) { if (!ARG0_IS_I(log_flag)) return (TCL_ERROR); argc--; argv++; }
  if( argc>0 ) { if (!ARG0_IS_I(int_flag)) return (TCL_ERROR); argc--; argv++; }

  /* if not given set defaults */
  if(r_max == -1.) r_max = min_box_l/2.0;
  if(r_bins < 0 )  r_bins = n_total_particles / 20;

  /* give back what you do */
  Tcl_AppendResult(interp, "{ analyze distribution { ", (char *)NULL);
  for(i=0; i<p1.max; i++) { 
    sprintf(buffer,"%d ",p1.e[i]);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
  }
  Tcl_AppendResult(interp,"} { ", (char *)NULL);
  for(i=0; i<p2.max; i++) {
    sprintf(buffer,"%d ",p2.e[i]);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
  }
  sprintf(buffer,"} %f %f %d %d %d",r_min,r_max,r_bins,log_flag,int_flag);
  Tcl_AppendResult(interp, buffer," }", (char *)NULL);
  /* some sanity checks */
  if(r_min < 0.0 || (log_flag==1 && r_min ==0.0 )) return TCL_ERROR;
  if(r_max <= r_min) return TCL_ERROR;
  if(r_bins < 1) return TCL_ERROR;
  /* calculate distribution */
  distribution = malloc(r_bins*sizeof(double));
  updatePartCfg(WITHOUT_BONDS);
  calc_part_distribution(p1.e, p1.max, p2.e, p2.max, r_min, r_max, r_bins, log_flag,&low,distribution);
  if(int_flag==1) {
    distribution[0] += low;
    for(i=0; i<r_bins-1; i++) distribution[i+1] += distribution[i]; 
  }
  /* append result */
  {
    double log_fac=0.0, bin_width=0.0, r=0.0;
    if(log_flag == 1) {
      log_fac       = pow((r_max/r_min),(1.0/(double)r_bins));
      r = r_min * sqrt(log_fac);
    } 
    else {
      bin_width     = (r_max-r_min) / (double)r_bins;
      r = r_min + bin_width/2.0;
    }
    Tcl_AppendResult(interp, " {\n", (char *)NULL);
    for(i=0; i<r_bins; i++) {
      sprintf(buffer,"%f %f",r,distribution[i]);
      Tcl_AppendResult(interp, "{ ",buffer," }\n", (char *)NULL);
      if(log_flag == 1) r *= log_fac; else r += bin_width;
    }
    Tcl_AppendResult(interp, "}\n", (char *)NULL);
  }
  free(distribution);
  return (TCL_OK);
}


static int parse_rdf(Tcl_Interp *interp, int average, int argc, char **argv)
{
  /* 'analyze rdf' (radial distribution function) */
  /************************************************/
  char buffer[2*TCL_DOUBLE_SPACE+TCL_INTEGER_SPACE+256];
  IntList p1,p2;
  double r_min=0, r_max=-1.0;
  int r_bins=-1, n_conf=1, i;
  double *rdf;

  init_intlist(&p1); init_intlist(&p2);

  if (argc < 2) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze {rdf|<rdf>|<rdf-intermol>} [<type_list> <type_list>]", (char *)NULL);
    return (TCL_ERROR);
  }

  if (!ARG0_IS_INTLIST(p1)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze {rdf|<rdf>|<rdf-intermol>} [<type_list> <type_list>]", (char *)NULL);
    return (TCL_ERROR);
  }
  if (!ARG1_IS_INTLIST(p2)) {
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "usage: analyze {rdf|<rdf>|<rdf-intermol>} [<type_list> <type_list>]", (char *)NULL);
    return (TCL_ERROR);
  }
  argc-=2; argv+=2;

  if( argc>0 ) { if (!ARG0_IS_D(r_min)) return (TCL_ERROR); argc--; argv++; }
  if( argc>0 ) { if (!ARG0_IS_D(r_max)) return (TCL_ERROR); argc--; argv++; }
  if( argc>0 ) { if (!ARG0_IS_I(r_bins)) return (TCL_ERROR); argc--; argv++; }
  if(average)
    {
      if (n_configs == 0) {
	Tcl_AppendResult(interp, "no configurations found! ", (char *)NULL);
	Tcl_AppendResult(interp, "Use 'analyze append' to save some, or 'analyze rdf' to only look at current RDF!", (char *)NULL);
	return TCL_ERROR;
      }
      if( argc>0 ) {
	if (!ARG0_IS_I(n_conf)) return (TCL_ERROR); argc--; argv++;
      }
      else
        n_conf  = n_configs;
    }

  /* if not given use default */
  if(r_max  == -1.0)  r_max = min_box_l/2.0;
  if(r_bins == -1  )  r_bins = n_total_particles / 20;

  /* give back what you do */
  if(average==0)
    Tcl_AppendResult(interp, "{ analyze rdf { ", (char *)NULL);
  else if(average==1)
    Tcl_AppendResult(interp, "{ analyze <rdf> { ", (char *)NULL);
  else if(average==2)
    Tcl_AppendResult(interp, "{ analyze <rdf-intermol> { ", (char *)NULL);
  else
    {
      Tcl_AppendResult(interp, "WRONG PARAMETER PASSED ", (char *)NULL);
      return TCL_ERROR;
    }

  for(i=0; i<p1.max; i++) {
    sprintf(buffer,"%d ",p1.e[i]);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
  }
  Tcl_AppendResult(interp,"} { ", (char *)NULL);
  for(i=0; i<p2.max; i++) {
    sprintf(buffer,"%d ",p2.e[i]);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
  }
  sprintf(buffer,"} %f %f %d",r_min,r_max,r_bins);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  if(average) {
    sprintf(buffer," %d",n_conf);
    Tcl_AppendResult(interp, buffer, " }",(char *)NULL);
  }
  else
    Tcl_AppendResult(interp, " }", (char *)NULL);
  rdf = malloc(r_bins*sizeof(double));

  updatePartCfg(WITHOUT_BONDS);

  if(average==0)
    calc_rdf(p1.e, p1.max, p2.e, p2.max, r_min, r_max, r_bins, rdf);
  else if(average==1)
    calc_rdf_av(p1.e, p1.max, p2.e, p2.max, r_min, r_max, r_bins, rdf, n_conf);
  else if(average==2)
    calc_rdf_intermol_av(p1.e, p1.max, p2.e, p2.max, r_min, r_max, r_bins, rdf, n_conf);
  else ;

  /* append result */
  {
    double bin_width=0.0, r=0.0;
    bin_width     = (r_max-r_min) / (double)r_bins;
    r = r_min + bin_width/2.0;
    Tcl_AppendResult(interp, " {\n", (char *)NULL);
    for(i=0; i<r_bins; i++) {
      sprintf(buffer,"%f %f",r,rdf[i]);
      Tcl_AppendResult(interp, "{ ",buffer," }\n", (char *)NULL);
      r += bin_width;
    }
    Tcl_AppendResult(interp, "}\n", (char *)NULL);
  }
  free(rdf);
  return (TCL_OK);
}


int parse_structurefactor(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze { stucturefactor } <type> <order>' */
  /***********************************************************************************************************/
  char buffer[2*TCL_DOUBLE_SPACE+4];
  int i, type, order;
  double qfak, *sf;
  if (argc < 2) {
    Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze structurefactor <type> <order> [<chain_start> <n_chains> <chain_length>]",
		     (char *)NULL);
    return (TCL_ERROR);
  } else {
    if (!ARG0_IS_I(type))
      return (TCL_ERROR);
    if (!ARG1_IS_I(order))
      return (TCL_ERROR);
    argc-=2; argv+=2;
  }
  updatePartCfg(WITHOUT_BONDS);
  analyze_structurefactor(type, order, &sf); 
  
  qfak = 2.0*PI/box_l[0];
  for(i=1; i<=order*order+1; i++) { 
    if (sf[i]>1e-6) sprintf(buffer,"{%f %f} ",qfak*sqrt(i),sf[i]); Tcl_AppendResult(interp, buffer, (char *)NULL); }
  free(sf);
  return (TCL_OK);
}

/****************************************************************************************
 *                                 parser for config storage stuff
 ****************************************************************************************/

static int parse_append(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze append' */
  /********************/
  char buffer[2*TCL_INTEGER_SPACE+256];

  if (argc != 0) { Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze append", (char *)NULL); return TCL_ERROR; }
  if (n_total_particles == 0) {
    Tcl_AppendResult(interp,"No particles to append! Use 'part' to create some, or 'analyze configs' to submit a bunch!",(char *) NULL); 
    return (TCL_ERROR); }
  if ((n_configs > 0) && (n_part_conf != n_total_particles)) {
    sprintf(buffer,"All configurations stored must have the same length (previously: %d, now: %d)!", n_part_conf, n_total_particles);
    Tcl_AppendResult(interp,buffer,(char *) NULL); return (TCL_ERROR); 
  }
  if (!sortPartCfg()) { Tcl_AppendResult(interp, "for analyze, store particles consecutively starting with 0.",(char *) NULL); return (TCL_ERROR); }
  analyze_append();
  sprintf(buffer,"%d",n_configs); Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_OK;
}

static int parse_push(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze push [<size>]' */
  /*****************************/
  char buffer[2*TCL_INTEGER_SPACE+256];
  int i, j;

  if (n_total_particles == 0) {
    Tcl_AppendResult(interp,"No particles to append! Use 'part' to create some, or 'analyze configs' to submit a bunch!",(char *) NULL); 
    return (TCL_ERROR); }
  if ((n_configs > 0) && (n_part_conf != n_total_particles)) {
    sprintf(buffer,"All configurations stored must have the same length (previously: %d, now: %d)!", n_part_conf, n_total_particles);
    Tcl_AppendResult(interp,buffer,(char *) NULL); return (TCL_ERROR); 
  }
  if (!sortPartCfg()) { Tcl_AppendResult(interp, "for analyze, store particles consecutively starting with 0.",(char *) NULL); return (TCL_ERROR); }
  if (argc == 1) { 
    if(!ARG0_IS_I(i)) return (TCL_ERROR);
    if (n_configs < i) analyze_append(); else analyze_push();
    if (n_configs > i) for(j=0; j < n_configs-i; j++) analyze_remove(0);
  }
  else if (argc != 0) { Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze push [<size>]", (char *)NULL); return TCL_ERROR; }
  else if (n_configs > 0) analyze_push();
  else analyze_append();
  sprintf(buffer,"%d",n_configs); Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_OK;
}

static int parse_replace(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze replace <index>' */
  /*****************************/
  char buffer[2*TCL_INTEGER_SPACE+256];
  int i;

  if (argc != 1) { Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze replace <index>", (char *)NULL); return TCL_ERROR; }
  if (n_total_particles == 0) {
    Tcl_AppendResult(interp,"No particles to append! Use 'part' to create some, or 'analyze configs' to submit a bunch!",(char *) NULL); 
    return (TCL_ERROR); }
  if ((n_configs > 0) && (n_part_conf != n_total_particles)) {
    sprintf(buffer,"All configurations stored must have the same length (previously: %d, now: %d)!", n_part_conf, n_total_particles);
    Tcl_AppendResult(interp,buffer,(char *) NULL); return (TCL_ERROR); 
  }
  if (!sortPartCfg()) { Tcl_AppendResult(interp, "for analyze, store particles consecutively starting with 0.",(char *) NULL); return (TCL_ERROR); }
  if (!ARG0_IS_I(i)) return (TCL_ERROR);
  if((n_configs == 0) && (i==0)) analyze_append();
  else if ((n_configs == 0) && (i!=0)) {
    Tcl_AppendResult(interp, "Nice try, but there are no stored configurations that could be replaced!", (char *)NULL); return TCL_ERROR; }
  else if((i < 0) || (i > n_configs-1)) {
    sprintf(buffer,"Index %d out of range (must be in [0,%d])!",i,n_configs-1);
    Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_ERROR; }
  else analyze_replace(i);
  sprintf(buffer,"%d",n_configs); Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_OK;
}

static int parse_remove(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze remove [<index>]' */
  /******************************/
  char buffer[2*TCL_INTEGER_SPACE+256];
  int i;

  if (!sortPartCfg()) { Tcl_AppendResult(interp, "for analyze, store particles consecutively starting with 0.",(char *) NULL); return (TCL_ERROR); }
  if (argc == 0) { for (i = n_configs-1; i >= 0; i--) analyze_remove(i); }
  else if (argc == 1) {
    if (!ARG0_IS_I(i)) return (TCL_ERROR);
    if(n_configs == 0) {
      Tcl_AppendResult(interp, "Nice try, but there are no stored configurations that could be removed!", (char *)NULL); return TCL_ERROR; }
    else if((i < 0) || (i > n_configs-1)) {
      sprintf(buffer,"Index %d out of range (must be in [0,%d])!",i,n_configs-1);
      Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_ERROR; }
    analyze_remove(i);
  }
  else {
    Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze remove [<index>]", (char *)NULL); return TCL_ERROR; 
  }
  sprintf(buffer,"%d",n_configs); Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_OK;
}

static int parse_configs(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze configs [ { <which> | <configuration> } ]' */
  /*******************************************************/
  char buffer[3*TCL_DOUBLE_SPACE+4*TCL_INTEGER_SPACE+256];
  double *tmp_config;
  int i, j;

  if (argc == 0) {
    for(i=0; i < n_configs; i++) {
      Tcl_AppendResult(interp,"{ ", (char *)NULL);
      for(j=0; j < n_part_conf; j++) {
	sprintf(buffer,"%f %f %f ",configs[i][3*j],configs[i][3*j+1],configs[i][3*j+2]);
	Tcl_AppendResult(interp, buffer,(char *)NULL);
      }
      Tcl_AppendResult(interp,"} ",(char *)NULL);
    }
    return (TCL_OK); }
  else if (argc == 1) {
    if (!ARG0_IS_I(i)) return (TCL_ERROR);
    if ((i<0) || (i>n_configs-1)) {
      sprintf(buffer,"The configs[%d] you requested does not exist, argument must be in [0,%d]!",i,n_configs-1);
      Tcl_AppendResult(interp,buffer,(char *)NULL); return TCL_ERROR; }
    for(j=0; j < n_part_conf; j++) {
      sprintf(buffer,"%f %f %f ",configs[i][3*j],configs[i][3*j+1],configs[i][3*j+2]);
      Tcl_AppendResult(interp, buffer,(char *)NULL);
    }
    return (TCL_OK); }
  else if ((argc == 3*n_part_conf) || (n_part_conf == 0)) {
    if ((n_part_conf == 0) && (argc % 3 == 0)) n_part_conf = argc/3;
    else if (argc != 3*n_part_conf) {
      sprintf(buffer,"Wrong # of args(%d)! Usage: analyze configs [x0 y0 z0 ... x%d y%d z%d]",argc,n_part_conf,n_part_conf,n_part_conf);
      Tcl_AppendResult(interp,buffer,(char *)NULL); return TCL_ERROR; }
    tmp_config = malloc(3*n_part_conf*sizeof(double));
    for(j=0; j < argc; j++)
      if (!ARG_IS_D(j, tmp_config[j])) return (TCL_ERROR);
    analyze_configs(tmp_config, n_part_conf); free(tmp_config);
    sprintf(buffer,"%d",n_configs); Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_OK;
  }
  /* else */
  sprintf(buffer,"Wrong # of args(%d)! Usage: analyze configs [x0 y0 z0 ... x%d y%d z%d]",argc,n_part_conf,n_part_conf,n_part_conf);
  Tcl_AppendResult(interp,buffer,(char *)NULL);
  return TCL_ERROR;
}

static int parse_activate(Tcl_Interp *interp, int argc, char **argv)
{
  /* 'analyze replace <index>' */
  /*****************************/
  char buffer[2*TCL_INTEGER_SPACE+256];
  int i;

  if (argc != 1) { Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze activate <index>", (char *)NULL); return TCL_ERROR; }
  if (n_total_particles == 0) {
    Tcl_AppendResult(interp,"No particles to append! Use 'part' to create some, or 'analyze configs' to submit a bunch!",(char *) NULL); 
    return (TCL_ERROR); }
  if ((n_configs > 0) && (n_part_conf != n_total_particles)) {
    sprintf(buffer,"All configurations stored must have the same length (previously: %d, now: %d)!", n_part_conf, n_total_particles);
    Tcl_AppendResult(interp,buffer,(char *) NULL); return (TCL_ERROR); 
  }
  if (!sortPartCfg()) { Tcl_AppendResult(interp, "for analyze, store particles consecutively starting with 0.",(char *) NULL); return (TCL_ERROR); }
  if (!ARG0_IS_I(i)) return (TCL_ERROR);
  if((n_configs == 0) && (i==0)) analyze_append();
  else if ((n_configs == 0) && (i!=0)) {
    Tcl_AppendResult(interp, "Nice try, but there are no stored configurations that could be replaced!", (char *)NULL); return TCL_ERROR; }
  else if((i < 0) || (i > n_configs-1)) {
    sprintf(buffer,"Index %d out of range (must be in [0,%d])!",i,n_configs-1);
    Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_ERROR; }
  else analyze_activate(i);
  sprintf(buffer,"%d",n_configs); Tcl_AppendResult(interp, buffer, (char *)NULL); return TCL_OK;
}


/****************************************************************************************
 *                                 main parser for analyze
 ****************************************************************************************/

int analyze(ClientData data, Tcl_Interp *interp, int argc, char **argv)
{
  int err = TCL_OK;
  if (argc < 2) {
    Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze <what> ...", (char *)NULL);
    return (TCL_ERROR);
  }

  if (ARG1_IS_S("set"))
    err = parse_analyze_set_topology(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("get_folded_positions"))
    err = parse_get_folded_positions(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("modes2d"))
    err = parse_modes2d(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("get_lipid_orients"))
    err = parse_get_lipid_orients(interp,argc-2,argv+2);
  else if (ARG1_IS_S("lipid_orient_order"))
    err = parse_lipid_orient_order(interp,argc-2,argv+2);
  else if (ARG1_IS_S("mindist"))
    err = parse_mindist(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("aggregation"))
    err = parse_aggregation(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("centermass"))
    err = parse_centermass(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("momentofinertiamatrix"))
    err = parse_momentofinertiamatrix(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("find_principal_axis"))
    err = parse_find_principal_axis(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("nbhood"))
    err = parse_nbhood(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("distto"))
    err = parse_distto(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("cell_gpb"))
    err = parse_cell_gpb(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("Vkappa"))
    err = parse_Vkappa(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("energy"))
    err = parse_and_print_energy(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("pressure"))
    err = parse_and_print_pressure(interp, argc - 2, argv + 2, 0);
  else if (ARG1_IS_S("p_inst"))
    err = parse_and_print_pressure(interp, argc - 2, argv + 2, 1);
  else if (ARG1_IS_S("bins"))
    err = parse_bins(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("p_IK1"))
    err = parse_and_print_p_IK1(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("re"))
    err = parse_re(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<re>"))
    err = parse_re(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("rg"))
    err = parse_rg(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<rg>"))
    err = parse_rg(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("rh"))
    err = parse_rh(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<rh>"))
    err = parse_rh(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("internal_dist"))
    err = parse_intdist(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<internal_dist>"))
    err = parse_intdist(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("bond_l"))
    err = parse_bond_l(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<bond_l>"))
    err = parse_bond_l(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("bond_dist"))
    err = parse_bond_dist(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<bond_dist>"))
    err = parse_bond_dist(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("g123"))
    err = parse_g123(interp, 1, argc - 2, argv + 2);    
  else if (ARG1_IS_S("<g1>"))
    err = parse_g_av(interp, 1, argc - 2, argv + 2);    
  else if (ARG1_IS_S("<g2>"))
    err = parse_g_av(interp, 2, argc - 2, argv + 2);    
  else if (ARG1_IS_S("<g3>"))
    err = parse_g_av(interp, 3, argc - 2, argv + 2);
  else if (ARG1_IS_S("formfactor"))
    err = parse_formfactor(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<formfactor>")) 
    err = parse_formfactor(interp, 1, argc - 2, argv + 2);    
  else if (ARG1_IS_S("necklace")) 
    err = parse_necklace_analyzation(interp, argc - 2, argv + 2);   
  else if (ARG1_IS_S("distribution"))
    err = parse_distribution(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("rdf"))
    err = parse_rdf(interp, 0, argc - 2, argv + 2);
  else if (ARG1_IS_S("<rdf>"))
    err = parse_rdf(interp, 1, argc - 2, argv + 2);
  else if (ARG1_IS_S("<rdf-intermol>"))
    err = parse_rdf(interp, 2, argc - 2, argv + 2);
  else if (ARG1_IS_S("rdfchain"))
    err = parse_rdfchain(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("structurefactor"))
    err = parse_structurefactor(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("append"))
    err = parse_append(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("push"))
    err = parse_push(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("replace"))
    err = parse_replace(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("activate"))
    err = parse_activate(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("remove"))
    err = parse_remove(interp, argc - 2, argv + 2);
  else if (ARG1_IS_S("stored")) {
    /* 'analyze stored' */
    /********************/
    char buffer[TCL_INTEGER_SPACE];
    if (argc != 2) {
      Tcl_AppendResult(interp, "Wrong # of args! Usage: analyze stored", (char *)NULL);
      err = TCL_ERROR; 
    }
    sprintf(buffer,"%d",n_configs);
    Tcl_AppendResult(interp, buffer, (char *)NULL);
    err = TCL_OK;
  }
  else if (ARG1_IS_S("configs"))
    err = parse_configs(interp, argc - 2, argv + 2);
  else {
    /* the default */
    /***************/
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, "The operation \"", argv[1],
		     "\" you requested is not implemented.", (char *)NULL);
    err = (TCL_ERROR);
  }
  return mpi_gather_runtime_errors(interp, err);
}
