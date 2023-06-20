#include <AMReX_VisMF.H>
#include <AMReX_AsyncOut.H>
#include <AMReX_PlotFileUtil.H>
#include <AMReX_FPC.H>
#include <AMReX_FabArrayUtility.H>

#ifdef AMREX_USE_EB
#include <AMReX_EBFabFactory.H>
#endif
#include <cmath>
#include "hdf5.h"

#ifdef AMREX_USE_HDF5_ZFP
#include "H5Zzfp_lib.h"
#include "H5Zzfp_props.h"
#endif

#ifdef AMREX_USE_HDF5_SZ
#include "H5Z_SZ.h"
#endif

#ifdef AMREX_USE_HDF5_SZ3
#include "hdf5_sz3/include/H5Z_SZ3.hpp"
#endif

#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

#define BSIZE 8

int gcd(int a, int b) {
    if (b == 0) {
        return a;
    } else {
        return gcd(b, a % b);
    }
}

namespace amrex {

#ifdef AMREX_USE_HDF5_ASYNC
hid_t es_id_g = 0;
#endif

static int CreateWriteHDF5AttrDouble(hid_t loc, const char *name, hsize_t n, const double *data)
{
    herr_t ret;
    hid_t attr, attr_space;
    hsize_t dims = n;

    attr_space = H5Screate_simple(1, &dims, NULL);

    attr = H5Acreate(loc, name, H5T_NATIVE_DOUBLE, attr_space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0) {
        printf("%s: Error with H5Acreate [%s]\n", __func__, name);
        return -1;
    }

    ret  = H5Awrite(attr, H5T_NATIVE_DOUBLE, (void*)data);
    if (ret < 0) {
        printf("%s: Error with H5Awrite [%s]\n", __func__, name);
        return -1;
    }
    H5Sclose(attr_space);
    H5Aclose(attr);
    return 1;
}

static int CreateWriteHDF5AttrInt(hid_t loc, const char *name, hsize_t n, const int *data)
{
    herr_t ret;
    hid_t attr, attr_space;
    hsize_t dims = n;

    attr_space = H5Screate_simple(1, &dims, NULL);

    attr = H5Acreate(loc, name, H5T_NATIVE_INT, attr_space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0) {
        printf("%s: Error with H5Acreate [%s]\n", __func__, name);
        return -1;
    }

    ret  = H5Awrite(attr, H5T_NATIVE_INT, (void*)data);
    if (ret < 0) {
        printf("%s: Error with H5Awrite [%s]\n", __func__, name);
        return -1;
    }
    H5Sclose(attr_space);
    H5Aclose(attr);
    return 1;
}

static int CreateWriteHDF5AttrString(hid_t loc, const char *name, const char* str)
{
    hid_t attr, atype, space;
    herr_t ret;

    BL_ASSERT(name);
    BL_ASSERT(str);

    space = H5Screate(H5S_SCALAR);
    atype = H5Tcopy(H5T_C_S1);
    H5Tset_size(atype, strlen(str)+1);
    H5Tset_strpad(atype,H5T_STR_NULLTERM);
    attr = H5Acreate(loc, name, atype, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0) {
        printf("%s: Error with H5Acreate [%s]\n", __func__, name);
        return -1;
    }

    ret = H5Awrite(attr, atype, str);
    if (ret < 0) {
        printf("%s: Error with H5Awrite[%s]\n", __func__, name);
        return -1;
    }

    H5Tclose(atype);
    H5Sclose(space);
    H5Aclose(attr);

    return 1;
}

#ifdef BL_USE_MPI
static void SetHDF5fapl(hid_t fapl, MPI_Comm comm)
#else
static void SetHDF5fapl(hid_t fapl)
#endif
{
#ifdef BL_USE_MPI
    H5Pset_fapl_mpio(fapl, comm, MPI_INFO_NULL);

    // Alignment and metadata block size
    int alignment = 16 * 1024 * 1024;
    int blocksize =  4 * 1024 * 1024;
    H5Pset_alignment(fapl, alignment, alignment);
    H5Pset_meta_block_size(fapl, blocksize);

    // Collective metadata ops
    H5Pset_coll_metadata_write(fapl, true);
    H5Pset_all_coll_metadata_ops(fapl, true);

    // Defer cache flush
    H5AC_cache_config_t cache_config;
    cache_config.version = H5AC__CURR_CACHE_CONFIG_VERSION;
    H5Pget_mdc_config(fapl, &cache_config);
    cache_config.set_initial_size = 1;
    cache_config.initial_size = 16 * 1024 * 1024;
    cache_config.evictions_enabled = 0;
    cache_config.incr_mode = H5C_incr__off;
    cache_config.flash_incr_mode = H5C_flash_incr__off;
    cache_config.decr_mode = H5C_decr__off;
    H5Pset_mdc_config (fapl, &cache_config);
#else
    H5Pset_fapl_sec2(fapl);
#endif

}


static void
WriteGenericPlotfileHeaderHDF5 (hid_t fid,
                               int nlevels,
                               const Vector<const MultiFab*>& mf,
                               const Vector<BoxArray> &bArray,
                               const Vector<std::string> &varnames,
                               const Vector<Geometry> &geom,
                               Real time,
                               const Vector<int> &level_steps,
                               const Vector<IntVect> &ref_ratio,
                               const std::string &versionName,
                               const std::string &levelPrefix,
                               const std::string &mfPrefix,
                               const Vector<std::string>& extra_dirs)
{
    BL_PROFILE("WriteGenericPlotfileHeaderHDF5()");

    BL_ASSERT(nlevels <= bArray.size());
    BL_ASSERT(nlevels <= geom.size());
    BL_ASSERT(nlevels <= ref_ratio.size()+1);
    BL_ASSERT(nlevels <= level_steps.size());

    int finest_level(nlevels - 1);

    CreateWriteHDF5AttrString(fid, "version_name", versionName.c_str());
    CreateWriteHDF5AttrString(fid, "plotfile_type", "VanillaHDF5");

    int ncomp = varnames.size();
    CreateWriteHDF5AttrInt(fid, "num_components", 1, &ncomp);

    char comp_name[32];
    for (int ivar = 0; ivar < varnames.size(); ++ivar) {
        sprintf(comp_name, "component_%d", ivar);
        CreateWriteHDF5AttrString(fid, comp_name, varnames[ivar].c_str());
    }

    int ndim = AMREX_SPACEDIM;
    CreateWriteHDF5AttrInt(fid, "dim", 1, &ndim);
    double cur_time = (double)time;
    CreateWriteHDF5AttrDouble(fid, "time", 1, &cur_time);
    CreateWriteHDF5AttrInt(fid, "finest_level", 1, &finest_level);


    int coord = (int) geom[0].Coord();
    CreateWriteHDF5AttrInt(fid, "coordinate_system", 1, &coord);

    hid_t grp;
    char level_name[128];
    double lo[AMREX_SPACEDIM], hi[AMREX_SPACEDIM], cellsizes[AMREX_SPACEDIM];

    // For VisIt Chombo plot
    CreateWriteHDF5AttrInt(fid, "num_levels", 1, &nlevels);
    grp = H5Gcreate(fid, "Chombo_global", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    CreateWriteHDF5AttrInt(grp, "SpaceDim", 1, &ndim);
    H5Gclose(grp);

    hid_t comp_dtype;

    comp_dtype = H5Tcreate (H5T_COMPOUND, 2 * AMREX_SPACEDIM * sizeof(int));
    if (1 == AMREX_SPACEDIM) {
        H5Tinsert (comp_dtype, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "hi_i", 1 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (2 == AMREX_SPACEDIM) {
        H5Tinsert (comp_dtype, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "lo_j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "hi_i", 2 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "hi_j", 3 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (3 == AMREX_SPACEDIM) {
        H5Tinsert (comp_dtype, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "lo_j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "lo_k", 2 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "hi_i", 3 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "hi_j", 4 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (comp_dtype, "hi_k", 5 * sizeof(int), H5T_NATIVE_INT);
    }

    for (int level = 0; level <= finest_level; ++level) {
        sprintf(level_name, "level_%d", level);
        /* sprintf(level_name, "%s%d", levelPrefix.c_str(), level); */
        grp = H5Gcreate(fid, level_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (grp < 0) {
            std::cout << "H5Gcreate [" << level_name << "] failed!" << std::endl;
            continue;
        }

        int ratio = 1;
        if (ref_ratio.size() > 0)
            ratio = (level == finest_level)? 1: ref_ratio[level][0];

        CreateWriteHDF5AttrInt(grp, "ref_ratio", 1, &ratio);

        for (int k = 0; k < AMREX_SPACEDIM; ++k) {
            cellsizes[k] = (double)geom[level].CellSize()[k];
        }
        // Visit has issues with vec_dx, and is ok with a single "dx" value
        CreateWriteHDF5AttrDouble(grp, "Vec_dx", AMREX_SPACEDIM, cellsizes);
        // For VisIt Chombo plot
        CreateWriteHDF5AttrDouble(grp, "dx", 1, &cellsizes[0]);

        for (int i = 0; i < AMREX_SPACEDIM; ++i) {
            lo[i] = (double)geom[level].ProbLo(i);
            hi[i] = (double)geom[level].ProbHi(i);
        }
        CreateWriteHDF5AttrDouble(grp, "prob_lo", AMREX_SPACEDIM, lo);
        CreateWriteHDF5AttrDouble(grp, "prob_hi", AMREX_SPACEDIM, hi);

        int domain[AMREX_SPACEDIM*2];
        Box tmp(geom[level].Domain());
        for (int i = 0; i < AMREX_SPACEDIM; ++i) {
            domain[i] = tmp.smallEnd(i);
            domain[i+AMREX_SPACEDIM] = tmp.bigEnd(i);
        }

        hid_t aid = H5Screate(H5S_SCALAR);
        hid_t domain_attr = H5Acreate(grp, "prob_domain", comp_dtype, aid, H5P_DEFAULT, H5P_DEFAULT);
        H5Awrite(domain_attr, comp_dtype, domain);
        H5Aclose(domain_attr);
        H5Sclose(aid);

        int type[AMREX_SPACEDIM];
        for (int i = 0; i < AMREX_SPACEDIM; ++i) {
            type[i] = (int)geom[level].Domain().ixType().test(i) ? 1 : 0;
        }
        CreateWriteHDF5AttrInt(grp, "domain_type", AMREX_SPACEDIM, type);

        CreateWriteHDF5AttrInt(grp, "steps", 1, &level_steps[level]);

        int ngrid = bArray[level].size();
        CreateWriteHDF5AttrInt(grp, "ngrid", 1, &ngrid);
        cur_time = (double)time;
        CreateWriteHDF5AttrDouble(grp, "time", 1, &cur_time);

        int ngrow = mf[level]->nGrow();
        CreateWriteHDF5AttrInt(grp, "ngrow", 1, &ngrow);

        /* hsize_t npts = ngrid*AMREX_SPACEDIM*2; */
        /* double *realboxes = new double [npts]; */
        /* for (int i = 0; i < bArray[level].size(); ++i) */
        /* { */
        /*     const Box &b(bArray[level][i]); */
        /*     RealBox loc = RealBox(b, geom[level].CellSize(), geom[level].ProbLo()); */
        /*     for (int n = 0; n < AMREX_SPACEDIM; ++n) { */
        /*         /1* HeaderFile << loc.lo(n) << ' ' << loc.hi(n) << '\n'; *1/ */
        /*         realboxes[i*AMREX_SPACEDIM*2 + n] = loc.lo(n); */
        /*         realboxes[i*AMREX_SPACEDIM*2 + AMREX_SPACEDIM + n] = loc.hi(n); */
        /*     } */
        /* } */
        /* CreateWriteDsetDouble(grp, "Boxes", npts, realboxes); */
        /* delete [] realboxes; */

        H5Gclose(grp);
    }

    H5Tclose(comp_dtype);
}

#ifdef AMREX_USE_HDF5_ASYNC
void async_vol_es_wait_close()
{
    size_t num_in_progress;
    hbool_t op_failed;
    if (es_id_g != 0) {
        H5ESwait(es_id_g, H5ES_WAIT_FOREVER, &num_in_progress, &op_failed);
        if (num_in_progress != 0)
            std::cout << "After H5ESwait, still has async operations in progress!" << std::endl;
        H5ESclose(es_id_g);
        es_id_g = 0;
        /* std::cout << "es_id_g closed!" << std::endl; */
    }
    return;
}
static void async_vol_es_wait()
{
    size_t num_in_progress;
    hbool_t op_failed;
    if (es_id_g != 0) {
        H5ESwait(es_id_g, H5ES_WAIT_FOREVER, &num_in_progress, &op_failed);
        if (num_in_progress != 0)
            std::cout << "After H5ESwait, still has async operations in progress!" << std::endl;
    }
    return;
}
#endif

void WriteMultiLevelPlotfileHDF5SingleDset (const std::string& plotfilename,
                                            int nlevels,
                                            const Vector<const MultiFab*>& mf,
                                            const Vector<std::string>& varnames,
                                            const Vector<Geometry>& geom,
                                            Real time,
                                            const Vector<int>& level_steps,
                                            const Vector<IntVect>& ref_ratio,
                                            const std::string &compression,
                                            const std::string &versionName,
                                            const std::string &levelPrefix,
                                            const std::string &mfPrefix,
                                            const Vector<std::string>& extra_dirs)
{
    BL_PROFILE("WriteMultiLevelPlotfileHDF5SingleDset");

    BL_ASSERT(nlevels <= mf.size());
    BL_ASSERT(nlevels <= geom.size());
    BL_ASSERT(nlevels <= ref_ratio.size()+1);
    BL_ASSERT(nlevels <= level_steps.size());
    BL_ASSERT(mf[0]->nComp() == varnames.size());

    int myProc(ParallelDescriptor::MyProc());
    int nProcs(ParallelDescriptor::NProcs());

#ifdef AMREX_USE_HDF5_ASYNC
    // For HDF5 async VOL, block and wait previous tasks have all completed
    if (es_id_g != 0) {
        async_vol_es_wait();
    }
    else {
        ExecOnFinalize(async_vol_es_wait_close);
        es_id_g = H5EScreate();
    }
#endif

    herr_t  ret;
    int finest_level = nlevels-1;
    int ncomp = mf[0]->nComp();
    /* double total_write_start_time(ParallelDescriptor::second()); */
    std::string filename(plotfilename + ".h5");

    // Write out root level metadata
    hid_t fapl, dxpl_col, dxpl_ind, dcpl_id, fid, grp, dcpl_id_lev;

    if(ParallelDescriptor::IOProcessor()) {
        BL_PROFILE_VAR("H5writeMetadata", h5dwm);
        // Create the HDF5 file
        fid = H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        if (fid < 0)
            FileOpenFailed(filename.c_str());

        Vector<BoxArray> boxArrays(nlevels);
        for(int level(0); level < boxArrays.size(); ++level) {
            boxArrays[level] = mf[level]->boxArray();
        }

        WriteGenericPlotfileHeaderHDF5(fid, nlevels, mf, boxArrays, varnames, geom, time, level_steps, ref_ratio, versionName, levelPrefix, mfPrefix, extra_dirs);
        H5Fclose(fid);
        BL_PROFILE_VAR_STOP(h5dwm);
    }

    ParallelDescriptor::Barrier();

    hid_t babox_id;
    babox_id = H5Tcreate (H5T_COMPOUND, 2 * AMREX_SPACEDIM * sizeof(int));
    if (1 == AMREX_SPACEDIM) {
        H5Tinsert (babox_id, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_i", 1 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (2 == AMREX_SPACEDIM) {
        H5Tinsert (babox_id, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "lo_j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_i", 2 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_j", 3 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (3 == AMREX_SPACEDIM) {
        H5Tinsert (babox_id, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "lo_j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "lo_k", 2 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_i", 3 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_j", 4 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_k", 5 * sizeof(int), H5T_NATIVE_INT);
    }

    hid_t center_id = H5Tcreate (H5T_COMPOUND, AMREX_SPACEDIM * sizeof(int));
    if (1 == AMREX_SPACEDIM) {
        H5Tinsert (center_id, "i", 0 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (2 == AMREX_SPACEDIM) {
        H5Tinsert (center_id, "i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (center_id, "j", 1 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (3 == AMREX_SPACEDIM) {
        H5Tinsert (center_id, "i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (center_id, "j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (center_id, "k", 2 * sizeof(int), H5T_NATIVE_INT);
    }

    fapl = H5Pcreate (H5P_FILE_ACCESS);
    dxpl_col = H5Pcreate(H5P_DATASET_XFER);
    dxpl_ind = H5Pcreate(H5P_DATASET_XFER);

#ifdef BL_USE_MPI
    SetHDF5fapl(fapl, ParallelDescriptor::Communicator());
    H5Pset_dxpl_mpio(dxpl_col, H5FD_MPIO_COLLECTIVE);
#else
    SetHDF5fapl(fapl);
#endif

    dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_fill_time(dcpl_id, H5D_FILL_TIME_NEVER);

#if (defined AMREX_USE_HDF5_ZFP) || (defined AMREX_USE_HDF5_SZ) || (defined AMREX_USE_HDF5_SZ3)
    const char *chunk_env = NULL;
    std::string mode_env, value_env;
    double comp_value = -1.0;
    hsize_t chunk_dim[1] = {98304};

    chunk_env = getenv("HDF5_CHUNK_SIZE");
    if (chunk_env != NULL)
        chunk_dim[0] = atoi(chunk_env);

    H5Pset_chunk(dcpl_id, 1, chunk_dim);
    H5Pset_alloc_time(dcpl_id, H5D_ALLOC_TIME_INCR);

    std::string::size_type pos = compression.find('@');
    if (pos != std::string::npos) {
        mode_env = compression.substr(0, pos);
        value_env = compression.substr(pos+1);
        if (!value_env.empty()) {
            comp_value = atof(value_env.c_str());
        }
    }

#ifdef AMREX_USE_HDF5_ZFP
    pos = compression.find("ZFP");
    if (pos != std::string::npos) {
        ret = H5Z_zfp_initialize();
        if (ret < 0) amrex::Abort("ZFP initialize failed!");
    }
#endif

// #ifdef AMREX_USE_HDF5_SZ
//     pos = compression.find("SZ");
//     if (pos != std::string::npos) {
//         ret = H5Z_SZ_Init((char*)value_env.c_str());
//         if (ret < 0) {
//             std::cout << "SZ config file:" << value_env.c_str() << std::endl;
//             amrex::Abort("SZ initialize failed, check SZ config file!");
//         }
//     }
// #endif

    if (!mode_env.empty() && mode_env != "None") {
        if (mode_env == "ZLIB")
            H5Pset_deflate(dcpl_id, (int)comp_value);
#ifdef AMREX_USE_HDF5_ZFP
        else if (mode_env == "ZFP_RATE")
            H5Pset_zfp_rate(dcpl_id, comp_value);
        else if (mode_env == "ZFP_PRECISION")
            H5Pset_zfp_precision(dcpl_id, (unsigned int)comp_value);
        else if (mode_env == "ZFP_ACCURACY")
            H5Pset_zfp_accuracy(dcpl_id, comp_value);
        else if (mode_env == "ZFP_REVERSIBLE")
            H5Pset_zfp_reversible(dcpl_id);
        else if (mode_env == "ZLIB")
            H5Pset_deflate(dcpl_id, (int)comp_value);
#endif

        if (ParallelDescriptor::MyProc() == 0) {
            std::cout << "\nHDF5 plotfile using " << mode_env << std::endl;
            // << ", " <<
            //     value_env << ", " << chunk_dim[0] << std::endl;
        }
    }
#endif

    BL_PROFILE_VAR("H5writeAllLevel", h5dwd);

    // All process open the file
#ifdef AMREX_USE_HDF5_ASYNC
    // Only use async for writing actual data
    fid = H5Fopen_async(filename.c_str(), H5F_ACC_RDWR, fapl, es_id_g);
#else
    fid = H5Fopen(filename.c_str(), H5F_ACC_RDWR, fapl);
#endif
    if (fid < 0)
        FileOpenFailed(filename.c_str());

    auto whichRD = FArrayBox::getDataDescriptor();
    bool doConvert(*whichRD != FPC::NativeRealDescriptor());
    int whichRDBytes(whichRD->numBytes());

    // Write data for each level
    char level_name[32];
    for (int level = 0; level <= finest_level; ++level) {
        sprintf(level_name, "level_%d", level);
#ifdef AMREX_USE_HDF5_ASYNC
        grp = H5Gopen_async(fid, level_name, H5P_DEFAULT, es_id_g);
#else
        grp = H5Gopen(fid, level_name, H5P_DEFAULT);
#endif
        if (grp < 0) { std::cout << "H5Gopen [" << level_name << "] failed!" << std::endl; break; }

        // Get the boxes assigned to all ranks and calculate their offsets and sizes
        Vector<int> procMap = mf[level]->DistributionMap().ProcessorMap();
        const BoxArray& grids = mf[level]->boxArray();
        hid_t boxdataset, boxdataspace;
        hid_t offsetdataset, offsetdataspace;
        hid_t centerdataset, centerdataspace;
        std::string bdsname("boxes");
        std::string odsname("data:offsets=0");
        std::string centername("boxcenter");
        std::string dataname("data:datatype=0");
        hsize_t  flatdims[1];
        flatdims[0] = grids.size();

        flatdims[0] = grids.size();
        boxdataspace = H5Screate_simple(1, flatdims, NULL);

#ifdef AMREX_USE_HDF5_ASYNC
        boxdataset = H5Dcreate_async(grp, bdsname.c_str(), babox_id, boxdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es_id_g);
#else
        boxdataset = H5Dcreate(grp, bdsname.c_str(), babox_id, boxdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
        if (boxdataset < 0) { std::cout << "H5Dcreate [" << bdsname << "] failed!" << std::endl; break; }


        // Create a boxarray sorted by rank
        std::map<int, Vector<Box> > gridMap;
        for(int i(0); i < grids.size(); ++i) {
            int gridProc(procMap[i]);
            Vector<Box> &boxesAtProc = gridMap[gridProc];
            boxesAtProc.push_back(grids[i]);
        }

        BoxArray sortedGrids(grids.size());
        Vector<int> sortedProcs(grids.size());
        int bIndex(0);
        Vector<int> boxOffsets(nProcs, 0);
        unsigned int boxTotalOffset(0);
        for(auto it = gridMap.begin(); it != gridMap.end(); ++it) {
            int proc = it->first;
            Vector<Box> &boxesAtProc = it->second;
            for(int ii(0); ii < boxesAtProc.size(); ++ii) {
                sortedGrids.set(bIndex, boxesAtProc[ii]);
                sortedProcs[bIndex] = proc;
                ++bIndex;
            }
            boxOffsets[proc] = boxTotalOffset;
            boxTotalOffset += boxesAtProc.size();
        }

        Vector<int> newMap(grids.size());
        for(int i(0); i < grids.size(); ++i) {
            newMap[boxOffsets[procMap[i]]] = i;
            boxOffsets[procMap[i]]++;
        }

        // dcdc-output procMap & sortedProcs
        // if (level == finest_level) {
            if(ParallelDescriptor::IOProcessor()) {
                std::cout << "procMap: ";
                for (auto i = procMap.begin(); i != procMap.end(); ++i)
                    std::cout << *i << " ";
                std::cout << std::endl;

                std::cout << "sortedProcs: ";
                for (auto i = sortedProcs.begin(); i != sortedProcs.end(); ++i)
                    std::cout << *i << " ";
                std::cout << std::endl;

                // //dcdc-output boxOffsets
                // std::cout << "boxOffsets: ";
                // for (auto i = boxOffsets.begin(); i != boxOffsets.end(); ++i)
                //     std::cout << *i << " ";
                // std::cout << std::endl;

                // //dcdc-output newMap
                // std::cout << "newMap: ";
                // for (auto i = newMap.begin(); i != newMap.end(); ++i)
                //     std::cout << *i << " ";
                // std::cout << std::endl;

            }
        // }



        hsize_t  oflatdims[1];
        oflatdims[0] = sortedGrids.size() + 1;
        offsetdataspace = H5Screate_simple(1, oflatdims, NULL);
#ifdef AMREX_USE_HDF5_ASYNC
        offsetdataset = H5Dcreate_async(grp, odsname.c_str(), H5T_NATIVE_LLONG, offsetdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es_id_g);
#else
        offsetdataset = H5Dcreate(grp, odsname.c_str(), H5T_NATIVE_LLONG, offsetdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
        if(offsetdataset < 0) { std::cout << "create offset dataset failed! ret = " << offsetdataset << std::endl; break;}

        hsize_t centerdims[1];
        centerdims[0]   = sortedGrids.size() ;
        centerdataspace = H5Screate_simple(1, centerdims, NULL);
#ifdef AMREX_USE_HDF5_ASYNC
        centerdataset = H5Dcreate_async(grp, centername.c_str(), center_id, centerdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es_id_g);
#else
        centerdataset = H5Dcreate(grp, centername.c_str(), center_id, centerdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
        if(centerdataset < 0) { std::cout << "Create center dataset failed! ret = " << centerdataset << std::endl; break;}

        Vector<unsigned long long> offsets(sortedGrids.size() + 1, 0);
        unsigned long long currentOffset(0L);
        for(int b(0); b < sortedGrids.size(); ++b) {
            offsets[b] = currentOffset;
            currentOffset += sortedGrids[b].numPts() * ncomp;
        }
        offsets[sortedGrids.size()] = currentOffset;

        Vector<unsigned long long> procOffsets(nProcs, 0);
        Vector<unsigned long long> procBufferSize(nProcs, 0);
        Vector<unsigned long long> realProcBufferSize(nProcs, 0);
        unsigned long long totalOffset(0);
        for(auto it = gridMap.begin(); it != gridMap.end(); ++it) {
            int proc = it->first;
            Vector<Box> &boxesAtProc = it->second;
            realProcBufferSize[proc] = 0L;
            for(int b(0); b < boxesAtProc.size(); ++b) {
                realProcBufferSize[proc] += boxesAtProc[b].numPts();
            }
            /* if (level == 2) { */
            /*     fprintf(stderr, "Rank %d: level %d, proc %d, offset %ld, size %ld, all size %ld\n", */
            /*             myProc, level, proc, procOffsets[proc], procBufferSize[proc], totalOffset); */
            /* } */
        }

        //dcdc find maxBuf
        unsigned long long maxBuf = *max_element(realProcBufferSize.begin(), realProcBufferSize.end());
        if(ParallelDescriptor::IOProcessor())
            std::cout << "maxBuf: " << maxBuf << std::endl;
        H5Pset_chunk(dcpl_id, 1, &maxBuf);

        //dcdc fill
        // for(auto it = gridMap.begin(); it != gridMap.end(); ++it) {
        //     int proc = it->first;
        //     procOffsets[proc] = totalOffset;
        //     procBufferSize[proc] = 0L;
        //     procBufferSize[proc] += maxBuf*ncomp;
        //     totalOffset += procBufferSize[proc];
        // }

        int nRealProc(0);
        for(int i=0; i<nProcs; ++i) {
            if (realProcBufferSize[i] > 0){
                procOffsets[i] = totalOffset;
                procBufferSize[i] = 0L;
                procBufferSize[i] = maxBuf*ncomp;
                totalOffset += procBufferSize[i];
                nRealProc++;
            }
        }


        /*dcdc-output procOffsets*/
        if(ParallelDescriptor::IOProcessor()) {
            std::cout << "procOffsets: ";
            for (auto i = procOffsets.begin(); i != procOffsets.end(); ++i)
                std::cout << *i << " ";
            std::cout << std::endl;
        }




        /*dcdc output box*/
        // unsigned long long totalBoxOffset(0);
        // Vector<unsigned long long> myBoxOffsets(gridMap[myProc].size(), 0);
        // std::cout << "box on proc " << myProc << std::endl;
        // for (int i(0); i < gridMap[myProc].size(); ++i) {
        //     for(int j(0); j < AMREX_SPACEDIM; ++j) {
        //         std::cout << gridMap[myProc][i].smallEnd(j) << " " << gridMap[myProc][i].bigEnd(j) << std::endl;
        //     }
        //     std::cout << gridMap[myProc][i].numPts() << std::endl;
        //     myBoxOffsets[i] = totalBoxOffset;
        //     totalBoxOffset += gridMap[myProc][i].numPts();
        // }
        // std::cout << "myBoxOffsets: ";
        // for (auto i = myBoxOffsets.begin(); i != myBoxOffsets.end(); ++i)
        //     std::cout << *i << " ";
        // std::cout << std::endl;

        // std::cout << "totalBoxOffset: " << totalBoxOffset << std::endl;


        //dcdc write metadata

        if(ParallelDescriptor::IOProcessor()) {
            int vbCount(0);
            Vector<int> vbox(sortedGrids.size() * 2 * AMREX_SPACEDIM);
            Vector<int> centering(sortedGrids.size() * AMREX_SPACEDIM);
            for(int b(0); b < sortedGrids.size(); ++b) {
                for(int i(0); i < AMREX_SPACEDIM; ++i) {
                    vbox[(vbCount * 2 * AMREX_SPACEDIM) + i] = sortedGrids[b].smallEnd(i);
                    vbox[(vbCount * 2 * AMREX_SPACEDIM) + i + AMREX_SPACEDIM] = sortedGrids[b].bigEnd(i);
                    centering[vbCount * AMREX_SPACEDIM + i] = sortedGrids[b].ixType().test(i) ? 1 : 0;
                }
                ++vbCount;
            }

            // Only proc zero needs to write out this information
#ifdef AMREX_USE_HDF5_ASYNC
            ret = H5Dwrite_async(offsetdataset, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, dxpl_ind, &(offsets[0]), es_id_g);
#else
            ret = H5Dwrite(offsetdataset, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, dxpl_ind, &(offsets[0]));
#endif
            if(ret < 0) { std::cout << "Write offset dataset failed! ret = " << ret << std::endl; }

#ifdef AMREX_USE_HDF5_ASYNC
            ret = H5Dwrite_async(centerdataset, center_id, H5S_ALL, H5S_ALL, dxpl_ind, &(centering[0]), es_id_g);
#else
            ret = H5Dwrite(centerdataset, center_id, H5S_ALL, H5S_ALL, dxpl_ind, &(centering[0]));
#endif
            if(ret < 0) { std::cout << "Write center dataset failed! ret = " << ret << std::endl; }

#ifdef AMREX_USE_HDF5_ASYNC
            ret = H5Dwrite_async(boxdataset, babox_id, H5S_ALL, H5S_ALL, dxpl_ind, &(vbox[0]), es_id_g);
#else
            ret = H5Dwrite(boxdataset, babox_id, H5S_ALL, H5S_ALL, dxpl_ind, &(vbox[0]));
#endif
            if(ret < 0) { std::cout << "Write box dataset failed! ret = " << ret << std::endl; }
        } // end IOProcessor

        hsize_t hs_procsize[1], hs_allprocsize[1], ch_offset[1];

        ch_offset[0]       = procOffsets[myProc];          // ---- offset on this proc
        hs_procsize[0]     = procBufferSize[myProc];       // ---- size of buffer on this proc
        std::cout << " " << std::endl;
        //std::cout << "size of old buffer on proc " << myProc << ": " << procBufferSize[myProc] << std::endl;
        //std::cout << "size of buffer on proc " << myProc << ": " << realProcBufferSize[myProc] << std::endl;
        // hs_allprocsize[0]  = offsets[sortedGrids.size()];  // ---- size of buffer on all procs
        //dcdc change total buf size
        hs_allprocsize[0]     = maxBuf *ncomp * nRealProc ;       // ---- size of buffer on all procs

        hid_t dataspace    = H5Screate_simple(1, hs_allprocsize, NULL);
        hid_t memdataspace = H5Screate_simple(1, hs_procsize, NULL);


        /* fprintf(stderr, "Rank %d: level %d, offset %ld, size %ld, all size %ld\n", myProc, level, ch_offset[0], hs_procsize[0], hs_allprocsize[0]); */

        if (realProcBufferSize[myProc] == 0)
            H5Sselect_none(dataspace);
        else
            H5Sselect_hyperslab(dataspace, H5S_SELECT_SET, ch_offset, NULL, hs_procsize, NULL);


        BL_PROFILE_VAR("H5DwriteData", h5dwg);


        //dcdc-start
        auto preFileTime0 = amrex::second();
        MultiFab *tempmf = const_cast<MultiFab*>(mf[level]);
        Real bogus_flag=-1e200;
        if (level < finest_level)
        {
            MultiFab *highMf = const_cast<MultiFab*>(mf[level+1]);
            const BoxArray baf = BoxArray((*highMf).boxArray()).coarsen(ref_ratio[level]);
            for (MFIter mfi(*tempmf); mfi.isValid(); ++mfi)
            {
                FArrayBox& myFab = (*tempmf)[mfi];
                    std::vector< std::pair<int,Box> > isects = baf.intersections((*tempmf).boxArray()[mfi.index()]);


                for (int ii = 0; ii < isects.size(); ii++){
                    myFab.setVal(bogus_flag,isects[ii].second,0,ncomp);
                }
            }
        }


        int bbb=16;
        std::ifstream bFile("bbb.txt");
        if (bFile.is_open()) {
            bFile >> bbb;
            bFile.close();
        } else {
            std::cerr << "Unable to open the bbb file." << std::endl;
        }
        int bSize =  bbb*std::pow(2,(double)level);

        //std::cout << "bSize: " << bSize << std::endl;

        Vector<Real> b_buffer(procBufferSize[myProc], 0);
        long long cnt = 0;
        long long tempCnt = 0;
        size_t bigX=1;

        // /*stack*/
         size_t unitBlkSize = bSize*bSize*bSize;
         if (realProcBufferSize[myProc]>0){
         bigX = cbrt(realProcBufferSize[myProc]/(unitBlkSize));
         //std::cout<< "init bigX: " << bigX << std::endl;
             size_t testBigZ = ((realProcBufferSize[myProc]/unitBlkSize) + (bigX*bigX) - 1) / (bigX*bigX);
             //std::cout<< "testBigZ: " << testBigZ << std::endl;

             while (testBigZ*bigX*bigX*unitBlkSize>(procBufferSize[myProc]/ncomp)){
                 bigX -= 1;
                 //std::cout << "protential bug, decreasing bigX to: " << bigX << std::endl;
                 testBigZ = ((realProcBufferSize[myProc]/unitBlkSize) + (bigX*bigX) - 1) / (bigX*bigX);
             }
         }
         std::cout<< "final bigX: " << bigX << std::endl;
         size_t b2Size = bSize*bSize;
         size_t big2X = bigX*bigX;


        for (int pp = 0; pp < ncomp; pp++) {
            for (MFIter mfi(*tempmf); mfi.isValid(); ++mfi)
            {
                Array4<Real> const& fab_array = (*tempmf).array(mfi);
                int ncomp = (*tempmf).nComp();
                const Box& box = mfi.validbox();

                const Dim3 lo = amrex::lbound(box);
                const Dim3 hi = amrex::ubound(box);

                //check mod
                if (level == finest_level) {
                    // std::cout << lo.x << " " << lo.y << " " << lo.z << std::endl;
                    if (lo.x%bSize!=0 || lo.y%bSize!=0 || lo.z%bSize!=0) {
                        std::cout << "wtwtwtwtwtwtwtwtwtwtwt" << std::endl;
                    }
                }

                /*stack*/
                 size_t cc=0;
                 size_t xx, yy, zz, bb, ii, jj, kk;
                 for (int z = 0; z < (hi.z-lo.z+1)/bSize; ++z){
                     for (int y = 0; y < (hi.y-lo.y+1)/bSize; ++y){
                         for (int x = 0; x < (hi.x-lo.x+1)/bSize; ++x){
                             // todo bb = 0
                             for (int k = lo.z+z*bSize; k < lo.z+z*bSize+bSize; ++k){
                                 for (int j =lo.y+y*bSize; j <lo.y+y*bSize+bSize; ++j){
                                     for (int i = lo.x+x*bSize; i <lo.x+x*bSize+bSize; ++i){
                                         if(fab_array(i,j,k,0) != bogus_flag) {
                                             cc = tempCnt/(unitBlkSize);
                                             zz = cc/big2X;
                                             yy = (cc - zz*big2X)/bigX;
                                             xx = cc -zz*big2X - yy*bigX;

                                             bb = tempCnt - unitBlkSize*cc;

                                             kk = bb/b2Size;
                                             jj = (bb - kk*b2Size)/bSize;
                                             ii = bb - kk*b2Size - jj*bSize;
                                             // if (b_buffer[(xx*bSize+ii) + (yy*bSize+jj)*bSize*bigX + (zz*bSize+kk)*big2X*b2Size] != 0)
                                             //     std::cout << b_buffer[(xx*bSize+ii) + (yy*bSize+jj)*bSize*bigX + (zz*bSize+kk)*big2X*b2Size] <<std::endl;
                                             b_buffer[(xx*bSize+ii) + (yy*bSize+jj)*bSize*bigX + (zz*bSize+kk)*big2X*b2Size+ pp*maxBuf] = fab_array(i,j,k,0);

                                             tempCnt++;
                                         }
                                     }
                                 }
                             }
                         }
                     }
                 }

                // /*nast*/
                    /*for (int z = 0; z < (hi.z-lo.z+1)/bSize; ++z)
                        for (int y = 0; y < (hi.y-lo.y+1)/bSize; ++y)
                            for (int x = 0; x < (hi.x-lo.x+1)/bSize; ++x)
                                for (int k = lo.z+z*bSize; k < lo.z+z*bSize+bSize; ++k)
                                    for (int j =lo.y+y*bSize; j <lo.y+y*bSize+bSize; ++j)
                                        for (int i = lo.x+x*bSize; i <lo.x+x*bSize+bSize; ++i){
                                            if(fab_array(i,j,k,0) != bogus_flag) {
                                                b_buffer[tempCnt + pp*maxBuf] = fab_array(i,j,k,pp);
                                                tempCnt++;
                                            }
                                            // // bs
                                            // else {
                                            //     b_buffer[cnt] = 0;
                                            //     cnt++;
                                            // }
                                        } */

                /*1d*/
                // for (int z = lo.z; z <= hi.z; ++z)
                //     for (int y = lo.y; y <= hi.y; ++y)
                //         for (int x = lo.x; x <= hi.x; ++x) {
                //             if(fab_array(x,y,z,0) != bogus_flag) {
                //                 b_buffer[cnt] = fab_array(x,y,z,0);
                //                 cnt++;
                //             }
                //             /*bs*/
                //             else {
                //                 cnt++;
                //             }
                //         }

            }
            cnt+=tempCnt;
            // std::cout << "tempCnt: " << tempCnt << std::endl;
            tempCnt=0;
        }
        if(ParallelDescriptor::IOProcessor()) {
            std::cout << "amrex using stack" << std::endl;
        }
        auto  preFileTime = amrex::second() - preFileTime0;
        const int IOProc2        = ParallelDescriptor::IOProcessorNumber();
        ParallelDescriptor::ReduceRealMax(preFileTime,IOProc2);
        amrex::Print() << "pre time = " << preFileTime << "  seconds" << "\n\n";


        /*out b_buffer to .bin*/
        // char b_name[64];
        // sprintf(b_name, "test/0_%d_%d.bin", level,myProc);
        // std::ofstream fout(b_name, std::ios::binary);
        // fout.write((char*)&b_buffer[0], cnt * sizeof(double));
        // fout.close();

        char meta_name[64];
        cnt = cnt/ncomp;
        //std::cout<< "cnt in buffer " << myProc << " : " << cnt << std::endl;

        /*stack*/
         size_t bigZ = ((cnt/unitBlkSize) + (bigX*bigX) - 1) / (bigX*bigX);
         //std::cout << "bigZ: " << bigZ << std::endl;
         cnt = bigZ*bigX*bigX*unitBlkSize;
         //std::cout << "fill cnt in proc: " << myProc << " : "<< cnt << std::endl;
         sprintf(meta_name, "meta/s_%d_%d.txt", level, myProc);
         std::ofstream sfile(meta_name);
         if (!sfile.is_open()) {
             std::cout << "Error opening metadata file\n";
             return;
         }
         sfile << bigX;
         sfile.close();
        /*stack*/

        if(ParallelDescriptor::IOProcessor()) {
            sprintf(meta_name, "meta/sp_%d.txt", level);
            std::ofstream spfile(meta_name);
            if (!spfile.is_open()) {
                std::cout << "Error opening metadata file\n";
                return;
            }
            for (auto i = sortedProcs.begin(); i != sortedProcs.end(); ++i)
                spfile << *i << " ";
            spfile.close();

            sprintf(meta_name, "meta/realp_%d.txt", level);
            std::ofstream rpfile(meta_name);
            if (!rpfile.is_open()) {
                std::cout << "Error opening metadata file\n";
                return;
            }
            rpfile  << nRealProc;
            rpfile.close();
        }

        unsigned long long checkFake = realProcBufferSize[myProc];
        realProcBufferSize[myProc] = cnt;

        if (level == finest_level) {
            char meta_name[128];
            sprintf(meta_name, "meta/meta_%d_%d.txt", level, myProc);
            std::ofstream outfile(meta_name);
            if (!outfile.is_open()) {
                std::cout << "Error opening metadata file\n";
                return;
            }
            outfile <<  realProcBufferSize[myProc];
            outfile.close();
            if(ParallelDescriptor::IOProcessor()) {

                sprintf(meta_name, "meta/f.txt", level);
                std::ofstream ffile(meta_name);
                if (!ffile.is_open()) {
                    std::cout << "Error opening metadata file\n";
                    return;
                }
                ffile << level;
                ffile.close();

                sprintf(meta_name, "meta/ncomp.txt", level);
                std::ofstream nfile(meta_name);
                if (!nfile.is_open()) {
                    std::cout << "Error opening metadata file\n";
                    return;
                }
                nfile << ncomp;
                nfile.close();

                sprintf(meta_name, "meta/p.txt", level);
                std::ofstream pfile(meta_name);
                if (!pfile.is_open()) {
                    std::cout << "Error opening metadata file\n";
                    return;
                }
                pfile << nProcs;
                pfile.close();

            }
        } else {
            char meta_name[128];
            sprintf(meta_name, "meta/meta_%d_%d.txt", level, myProc);
            std::ofstream outfile(meta_name);
            if (!outfile.is_open()) {
                std::cout << "Error opening metadata file\n";
                return;
            }
            if (checkFake != 0) {
                outfile <<  realProcBufferSize[myProc];
            } else {
                outfile << -1;
            }
            outfile.close();
        }

        double eb = 0.001;
        std::ifstream inputFile("eb.txt");
        if (inputFile.is_open()) {
            inputFile >> eb;
            inputFile.close();
        } else {
            std::cerr << "Unable to open the eb file." << std::endl;
        }

#ifdef AMREX_USE_HDF5_SZ
        if (mode_env == "SZ") {
            size_t cd_nelmts;
            unsigned int* cd_values = NULL;
            unsigned filter_config;
            SZ_errConfigToCdArray(&cd_nelmts, &cd_values, 1, bigX*bSize, eb, level, realProcBufferSize[myProc]);
            dcpl_id_lev = H5Pcopy(dcpl_id);
            H5Pset_filter(dcpl_id_lev, H5Z_FILTER_SZ, H5Z_FLAG_MANDATORY, cd_nelmts, cd_values);
        }
#endif

#ifdef AMREX_USE_HDF5_SZ3
        if (mode_env == "SZ") {
            size_t cd_nelmts;
            unsigned int* cd_values = NULL;
            unsigned filter_config;
            SZ_errConfigToCdArray(&cd_nelmts, &cd_values, 1, bigX*bSize, eb, level, realProcBufferSize[myProc]);
            dcpl_id_lev = H5Pcopy(dcpl_id);
            H5Pset_filter(dcpl_id_lev, H5Z_FILTER_SZ3, H5Z_FLAG_MANDATORY, cd_nelmts, cd_values);
        }
#endif

#ifdef AMREX_USE_HDF5_ASYNC
        hid_t dataset = H5Dcreate_async(grp, dataname.c_str(), H5T_NATIVE_DOUBLE, dataspace, H5P_DEFAULT, dcpl_id_lev, H5P_DEFAULT, es_id_g);
#else
        hid_t dataset = H5Dcreate(grp, dataname.c_str(), H5T_NATIVE_DOUBLE, dataspace, H5P_DEFAULT, dcpl_id_lev, H5P_DEFAULT);
#endif
        if(dataset < 0)
            std::cout << ParallelDescriptor::MyProc() << "create data failed!  ret = " << dataset << std::endl;

        auto dPlotFileTime0 = amrex::second();
#ifdef AMREX_USE_HDF5_ASYNC
        ret = H5Dwrite_async(dataset, H5T_NATIVE_DOUBLE, memdataspace, dataspace, dxpl_col, b_buffer.dataPtr(), es_id_g);
#else
        ret = H5Dwrite(dataset, H5T_NATIVE_DOUBLE, memdataspace, dataspace, dxpl_col, b_buffer.dataPtr());
#endif
        if(ret < 0) { std::cout << ParallelDescriptor::MyProc() << "Write data failed!  ret = " << ret << std::endl; break; }
        auto  dPlotFileTime = amrex::second() - dPlotFileTime0;
        const int IOProc        = ParallelDescriptor::IOProcessorNumber();
        ParallelDescriptor::ReduceRealMax(dPlotFileTime,IOProc);
        amrex::Print() << "real write time = " << dPlotFileTime << "  seconds" << "\n\n";

        BL_PROFILE_VAR_STOP(h5dwg);
        H5Pclose(dcpl_id_lev);
        H5Sclose(memdataspace);
        H5Sclose(dataspace);
        H5Sclose(offsetdataspace);
        H5Sclose(centerdataspace);
        H5Sclose(boxdataspace);

#ifdef AMREX_USE_HDF5_ASYNC
        H5Dclose_async(dataset, es_id_g);
        H5Dclose_async(offsetdataset, es_id_g);
        H5Dclose_async(centerdataset, es_id_g);
        H5Dclose_async(boxdataset, es_id_g);
        H5Gclose_async(grp, es_id_g);
#else
        H5Dclose(dataset);
        H5Dclose(offsetdataset);
        H5Dclose(centerdataset);
        H5Dclose(boxdataset);
        H5Gclose(grp);
#endif
    } // For group

    BL_PROFILE_VAR_STOP(h5dwd);

    H5Tclose(center_id);
    H5Tclose(babox_id);
    H5Pclose(fapl);
    H5Pclose(dxpl_col);
    H5Pclose(dxpl_ind);
    H5Pclose(dcpl_id);

#ifdef AMREX_USE_HDF5_ASYNC
    H5Fclose_async(fid, es_id_g);
#else
    H5Fclose(fid);
#endif


} // WriteMultiLevelPlotfileHDF5SingleDset

void WriteMultiLevelPlotfileHDF5MultiDset (const std::string& plotfilename,
                                           int nlevels,
                                           const Vector<const MultiFab*>& mf,
                                           const Vector<std::string>& varnames,
                                           const Vector<Geometry>& geom,
                                           Real time,
                                           const Vector<int>& level_steps,
                                           const Vector<IntVect>& ref_ratio,
                                           const std::string &compression,
                                           const std::string &versionName,
                                           const std::string &levelPrefix,
                                           const std::string &mfPrefix,
                                           const Vector<std::string>& extra_dirs)
{
    BL_PROFILE("WriteMultiLevelPlotfileHDF5MultiDset");

    BL_ASSERT(nlevels <= mf.size());
    BL_ASSERT(nlevels <= geom.size());
    BL_ASSERT(nlevels <= ref_ratio.size()+1);
    BL_ASSERT(nlevels <= level_steps.size());
    BL_ASSERT(mf[0]->nComp() == varnames.size());

    int myProc(ParallelDescriptor::MyProc());
    int nProcs(ParallelDescriptor::NProcs());

#ifdef AMREX_USE_HDF5_ASYNC
    // For HDF5 async VOL, block and wait previous tasks have all completed
    if (es_id_g != 0) {
        async_vol_es_wait();
    }
    else {
        ExecOnFinalize(async_vol_es_wait_close);
        es_id_g = H5EScreate();
    }
#endif

    herr_t  ret;
    int finest_level = nlevels-1;
    int ncomp = mf[0]->nComp();
    /* double total_write_start_time(ParallelDescriptor::second()); */
    std::string filename(plotfilename + ".h5");

    // Write out root level metadata
    hid_t fapl, dxpl_col, dxpl_ind, fid, grp, dcpl_id, dcpl_id_lev;

    if(ParallelDescriptor::IOProcessor()) {
        BL_PROFILE_VAR("H5writeMetadata", h5dwm);
        // Create the HDF5 file
        fid = H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        if (fid < 0)
            FileOpenFailed(filename.c_str());

        Vector<BoxArray> boxArrays(nlevels);
        for(int level(0); level < boxArrays.size(); ++level) {
            boxArrays[level] = mf[level]->boxArray();
        }

        WriteGenericPlotfileHeaderHDF5(fid, nlevels, mf, boxArrays, varnames, geom, time, level_steps, ref_ratio, versionName, levelPrefix, mfPrefix, extra_dirs);
        H5Fclose(fid);
        BL_PROFILE_VAR_STOP(h5dwm);
    }

    ParallelDescriptor::Barrier();

    hid_t babox_id;
    babox_id = H5Tcreate (H5T_COMPOUND, 2 * AMREX_SPACEDIM * sizeof(int));
    if (1 == AMREX_SPACEDIM) {
        H5Tinsert (babox_id, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_i", 1 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (2 == AMREX_SPACEDIM) {
        H5Tinsert (babox_id, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "lo_j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_i", 2 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_j", 3 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (3 == AMREX_SPACEDIM) {
        H5Tinsert (babox_id, "lo_i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "lo_j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "lo_k", 2 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_i", 3 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_j", 4 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (babox_id, "hi_k", 5 * sizeof(int), H5T_NATIVE_INT);
    }

    hid_t center_id = H5Tcreate (H5T_COMPOUND, AMREX_SPACEDIM * sizeof(int));
    if (1 == AMREX_SPACEDIM) {
        H5Tinsert (center_id, "i", 0 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (2 == AMREX_SPACEDIM) {
        H5Tinsert (center_id, "i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (center_id, "j", 1 * sizeof(int), H5T_NATIVE_INT);
    }
    else if (3 == AMREX_SPACEDIM) {
        H5Tinsert (center_id, "i", 0 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (center_id, "j", 1 * sizeof(int), H5T_NATIVE_INT);
        H5Tinsert (center_id, "k", 2 * sizeof(int), H5T_NATIVE_INT);
    }

    fapl = H5Pcreate (H5P_FILE_ACCESS);
    dxpl_col = H5Pcreate(H5P_DATASET_XFER);
    dxpl_ind = H5Pcreate(H5P_DATASET_XFER);

#ifdef BL_USE_MPI
    SetHDF5fapl(fapl, ParallelDescriptor::Communicator());
    H5Pset_dxpl_mpio(dxpl_col, H5FD_MPIO_COLLECTIVE);
#else
    SetHDF5fapl(fapl);
#endif

    dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_fill_time(dcpl_id, H5D_FILL_TIME_NEVER);

#if (defined AMREX_USE_HDF5_ZFP) || (defined AMREX_USE_HDF5_SZ) || (defined AMREX_USE_HDF5_SZ3)
    const char *chunk_env = NULL;
    std::string mode_env, value_env;
    double comp_value = -1.0;
    hsize_t chunk_dim = 32768;

    chunk_env = getenv("HDF5_CHUNK_SIZE");
    if (chunk_env != NULL)
        chunk_dim = atoi(chunk_env);

    H5Pset_chunk(dcpl_id, 1, &chunk_dim);
    H5Pset_alloc_time(dcpl_id, H5D_ALLOC_TIME_INCR);

    std::string::size_type pos = compression.find('@');
    if (pos != std::string::npos) {
        mode_env = compression.substr(0, pos);
        value_env = compression.substr(pos+1);
        if (!value_env.empty()) {
            comp_value = atof(value_env.c_str());
        }
    }

#ifdef AMREX_USE_HDF5_ZFP
    pos = compression.find("ZFP");
    if (pos != std::string::npos) {
        ret = H5Z_zfp_initialize();
        if (ret < 0) amrex::Abort("ZFP initialize failed!");
    }
#endif

// #ifdef AMREX_USE_HDF5_SZ
//     pos = compression.find("SZ");
//     if (pos != std::string::npos) {
//         ret = H5Z_SZ_Init((char*)value_env.c_str());
//         if (ret < 0) amrex::Abort("ZFP initialize failed, check SZ config file!");
//     }
// #endif

    if (!mode_env.empty() && mode_env != "None") {
        if (mode_env == "ZLIB")
            H5Pset_deflate(dcpl_id, (int)comp_value);
#ifdef AMREX_USE_HDF5_ZFP
        else if (mode_env == "ZFP_RATE")
            H5Pset_zfp_rate(dcpl_id, comp_value);
        else if (mode_env == "ZFP_PRECISION")
            H5Pset_zfp_precision(dcpl_id, (unsigned int)comp_value);
        else if (mode_env == "ZFP_ACCURACY")
            H5Pset_zfp_accuracy(dcpl_id, comp_value);
        else if (mode_env == "ZFP_REVERSIBLE")
            H5Pset_zfp_reversible(dcpl_id);
#endif

        if (ParallelDescriptor::MyProc() == 0) {
            std::cout << "\nHDF5 plotfile using " << mode_env << std::endl;
        }
    }
#endif

    BL_PROFILE_VAR("H5writeAllLevel", h5dwd);

    // All process open the file
#ifdef AMREX_USE_HDF5_ASYNC
    // Only use async for writing actual data
    fid = H5Fopen_async(filename.c_str(), H5F_ACC_RDWR, fapl, es_id_g);
#else
    fid = H5Fopen(filename.c_str(), H5F_ACC_RDWR, fapl);
#endif
    if (fid < 0)
        FileOpenFailed(filename.c_str());

    auto whichRD = FArrayBox::getDataDescriptor();
    bool doConvert(*whichRD != FPC::NativeRealDescriptor());
    int whichRDBytes(whichRD->numBytes());

    // Write data for each level
    char level_name[32];

    for (int level = 0; level <= finest_level; ++level) {
        sprintf(level_name, "level_%d", level);
#ifdef AMREX_USE_HDF5_ASYNC
        grp = H5Gopen_async(fid, level_name, H5P_DEFAULT, es_id_g);
#else
        grp = H5Gopen(fid, level_name, H5P_DEFAULT);
#endif
        if (grp < 0) { std::cout << "H5Gopen [" << level_name << "] failed!" << std::endl; break; }

        // Get the boxes assigned to all ranks and calculate their offsets and sizes
        Vector<int> procMap = mf[level]->DistributionMap().ProcessorMap();
        const BoxArray& grids = mf[level]->boxArray();
        hid_t boxdataset, boxdataspace;
        hid_t offsetdataset, offsetdataspace;
        hid_t centerdataset, centerdataspace;
        std::string bdsname("boxes");
        std::string odsname("data:offsets=0");
        std::string centername("boxcenter");
        hsize_t  flatdims[1];
        flatdims[0] = grids.size();

        flatdims[0] = grids.size();
        boxdataspace = H5Screate_simple(1, flatdims, NULL);

#ifdef AMREX_USE_HDF5_ASYNC
        boxdataset = H5Dcreate_async(grp, bdsname.c_str(), babox_id, boxdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es_id_g);
#else
        boxdataset = H5Dcreate(grp, bdsname.c_str(), babox_id, boxdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
        if (boxdataset < 0) { std::cout << "H5Dcreate [" << bdsname << "] failed!" << std::endl; break; }

        // Create a boxarray sorted by rank
        std::map<int, Vector<Box> > gridMap;
        for(int i(0); i < grids.size(); ++i) {
            int gridProc(procMap[i]);
            Vector<Box> &boxesAtProc = gridMap[gridProc];
            boxesAtProc.push_back(grids[i]);
        }
        BoxArray sortedGrids(grids.size());
        Vector<int> sortedProcs(grids.size());
        int bIndex(0);
        for(auto it = gridMap.begin(); it != gridMap.end(); ++it) {
            int proc = it->first;
            Vector<Box> &boxesAtProc = it->second;
            for(int ii(0); ii < boxesAtProc.size(); ++ii) {
                sortedGrids.set(bIndex, boxesAtProc[ii]);
                sortedProcs[bIndex] = proc;
                ++bIndex;
            }
        }

        hsize_t  oflatdims[1];
        oflatdims[0] = sortedGrids.size() + 1;
        offsetdataspace = H5Screate_simple(1, oflatdims, NULL);
#ifdef AMREX_USE_HDF5_ASYNC
        offsetdataset = H5Dcreate_async(grp, odsname.c_str(), H5T_NATIVE_LLONG, offsetdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es_id_g);
#else
        offsetdataset = H5Dcreate(grp, odsname.c_str(), H5T_NATIVE_LLONG, offsetdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
        if(offsetdataset < 0) { std::cout << "create offset dataset failed! ret = " << offsetdataset << std::endl; break;}

        hsize_t centerdims[1];
        centerdims[0]   = sortedGrids.size() ;
        centerdataspace = H5Screate_simple(1, centerdims, NULL);
#ifdef AMREX_USE_HDF5_ASYNC
        centerdataset = H5Dcreate_async(grp, centername.c_str(), center_id, centerdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT, es_id_g);
#else
        centerdataset = H5Dcreate(grp, centername.c_str(), center_id, centerdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
        if(centerdataset < 0) { std::cout << "Create center dataset failed! ret = " << centerdataset << std::endl; break;}

        Vector<unsigned long long> offsets(sortedGrids.size() + 1);
        unsigned long long currentOffset(0L);
        for(int b(0); b < sortedGrids.size(); ++b) {
            offsets[b] = currentOffset;
            /* currentOffset += sortedGrids[b].numPts() * ncomp; */
            currentOffset += sortedGrids[b].numPts();
        }
        offsets[sortedGrids.size()] = currentOffset;

        Vector<unsigned long long> procOffsets(nProcs);
        Vector<unsigned long long> procBufferSize(nProcs);
        Vector<unsigned long long> realProcBufferSize(nProcs, 0);
        for(auto it = gridMap.begin(); it != gridMap.end(); ++it) {
            int proc = it->first;
            Vector<Box> &boxesAtProc = it->second;
            realProcBufferSize[proc] = 0L;
            for(int b(0); b < boxesAtProc.size(); ++b) {
                realProcBufferSize[proc] += boxesAtProc[b].numPts();
            }
        }



        unsigned long long maxBuf = *max_element(realProcBufferSize.begin(), realProcBufferSize.end());
        if(ParallelDescriptor::IOProcessor())
            std::cout << "maxBuf: " << maxBuf << std::endl;
        H5Pset_chunk(dcpl_id, 1, &maxBuf);

        //dcdc fill
        unsigned long long totalOffset(0);
        for(auto it = gridMap.begin(); it != gridMap.end(); ++it) {
            int proc = it->first;
            procOffsets[proc] = totalOffset;
            procBufferSize[proc] = 0L;
            procBufferSize[proc] += maxBuf;
            totalOffset += procBufferSize[proc];
        }

         // //dcdc-output procOffsets
        if(ParallelDescriptor::IOProcessor()) {
            std::cout << "procOffsets: ";
            for (auto i = procOffsets.begin(); i != procOffsets.end(); ++i)
                std::cout << *i << " ";
            std::cout << std::endl;
        }

        if(ParallelDescriptor::IOProcessor()) {
            int vbCount(0);
            Vector<int> vbox(sortedGrids.size() * 2 * AMREX_SPACEDIM);
            Vector<int> centering(sortedGrids.size() * AMREX_SPACEDIM);
            for(int b(0); b < sortedGrids.size(); ++b) {
                for(int i(0); i < AMREX_SPACEDIM; ++i) {
                    vbox[(vbCount * 2 * AMREX_SPACEDIM) + i] = sortedGrids[b].smallEnd(i);
                    vbox[(vbCount * 2 * AMREX_SPACEDIM) + i + AMREX_SPACEDIM] = sortedGrids[b].bigEnd(i);
                    centering[vbCount * AMREX_SPACEDIM + i] = sortedGrids[b].ixType().test(i) ? 1 : 0;
                }
                ++vbCount;
            }

            // Only proc zero needs to write out this information
#ifdef AMREX_USE_HDF5_ASYNC
            ret = H5Dwrite_async(offsetdataset, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, dxpl_ind, &(offsets[0]), es_id_g);
#else
            ret = H5Dwrite(offsetdataset, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, dxpl_ind, &(offsets[0]));
#endif
            if(ret < 0) { std::cout << "Write offset dataset failed! ret = " << ret << std::endl; }

#ifdef AMREX_USE_HDF5_ASYNC
            ret = H5Dwrite_async(centerdataset, center_id, H5S_ALL, H5S_ALL, dxpl_ind, &(centering[0]), es_id_g);
#else
            ret = H5Dwrite(centerdataset, center_id, H5S_ALL, H5S_ALL, dxpl_ind, &(centering[0]));
#endif
            if(ret < 0) { std::cout << "Write center dataset failed! ret = " << ret << std::endl; }

#ifdef AMREX_USE_HDF5_ASYNC
            ret = H5Dwrite_async(boxdataset, babox_id, H5S_ALL, H5S_ALL, dxpl_ind, &(vbox[0]), es_id_g);
#else
            ret = H5Dwrite(boxdataset, babox_id, H5S_ALL, H5S_ALL, dxpl_ind, &(vbox[0]));
#endif
            if(ret < 0) { std::cout << "Write box dataset failed! ret = " << ret << std::endl; }
        } // end IOProcessor

        hsize_t hs_procsize[1], hs_allprocsize[1], ch_offset[1];

        ch_offset[0]       = procOffsets[myProc];          // ---- offset on this proc
        hs_procsize[0]     = procBufferSize[myProc];       // ---- size of buffer on this proc
        std::cout << "nComp()" << ncomp << std::endl;
        std::cout << "size of buffer on proc " << myProc << ": " << realProcBufferSize[myProc] << std::endl;
        hs_allprocsize[0]     = maxBuf * nProcs ; ;  // ---- size of buffer on all procs

        hid_t dataspace    = H5Screate_simple(1, hs_allprocsize, NULL);
        hid_t memdataspace = H5Screate_simple(1, hs_procsize, NULL);

        if (realProcBufferSize[myProc] == 0)
            H5Sselect_none(dataspace);
        else
            H5Sselect_hyperslab(dataspace, H5S_SELECT_SET, ch_offset, NULL, hs_procsize, NULL);

        Vector<Real> b_buffer(procBufferSize[myProc], -1.0);
        // const MultiFab* data;
        // std::unique_ptr<MultiFab> mf_tmp;
        // if (mf[level]->nGrowVect() != 0) {
        //     mf_tmp = std::make_unique<MultiFab>(mf[level]->boxArray(),
        //                                         mf[level]->DistributionMap(),
        //                                         mf[level]->nComp(), 0, MFInfo(),
        //                                         mf[level]->Factory());
        //     MultiFab::Copy(*mf_tmp, *mf[level], 0, 0, mf[level]->nComp(), 0);
        //     data = mf_tmp.get();
        // } else {
        //     data = mf[level];
        // }

        hid_t dataset;
        char dataname[64];

        BL_PROFILE_VAR("H5DwriteData", h5dwg);
        //dcdc-start
        MultiFab *tempmf = const_cast<MultiFab*>(mf[level]);
        Real bogus_flag=-1e200;
        if (level < finest_level)
        {
            MultiFab *highMf = const_cast<MultiFab*>(mf[level+1]);
            const BoxArray baf = BoxArray((*highMf).boxArray()).coarsen(ref_ratio[level]);
            for (MFIter mfi(*tempmf); mfi.isValid(); ++mfi)
            {
                FArrayBox& myFab = (*tempmf)[mfi];
                    std::vector< std::pair<int,Box> > isects = baf.intersections((*tempmf).boxArray()[mfi.index()]);


                for (int ii = 0; ii < isects.size(); ii++){
                    myFab.setVal(bogus_flag,isects[ii].second,0,ncomp);
                }
            }
        }

        int bSize = BSIZE;

        size_t bigX=0;
        // /*stack*/
        // size_t unitBlkSize = bSize*bSize*bSize;
        // bigX = cbrt(realProcBufferSize[myProc]/(unitBlkSize))/1.5;
        // size_t testBigZ = ((realProcBufferSize[myProc]/(bSize*bSize*bSize)) + (bigX*bigX) - 1) / (bigX*bigX);
        // std::cout<< "testBigZ: " << testBigZ << std::endl;
        // if (testBigZ*bigX*bigX*unitBlkSize>procBufferSize[myProc]){
        //     std::cout << "protential bug, set bigX to 1 " << testBigZ*bigX*bigX << std::endl;
        //     bigX = 1;
        // }
        // std::cout<< "bigX: " << bigX << std::endl;
        // size_t b2Size = bSize*bSize;
        // size_t big2X = bigX*bigX;

        for (int jj = 0; jj < ncomp; jj++) {

            long long cnt = 0;
            for (MFIter mfi(*tempmf); mfi.isValid(); ++mfi)
            {
                Array4<Real> const& fab_array = (*tempmf).array(mfi);
                int ncomp = (*tempmf).nComp();
                const Box& box = mfi.validbox();

                const Dim3 lo = amrex::lbound(box);
                const Dim3 hi = amrex::ubound(box);

                //check mod
                if (jj == 0 && level == finest_level) {
                    std::cout << lo.x << " " << lo.y << " " << lo.z << std::endl;
                    if (lo.x%bSize!=0 || lo.y%bSize!=0 || lo.z%bSize!=0) {
                        std::cout << "wtwtwtwtwtwtwtwtwtwtwt" << std::endl;
                    }
                }


                /*stack*/
                // size_t cc=0;
                // size_t xx, yy, zz, bb, ii, jjj, kk;
                // for (int z = 0; z < (hi.z-lo.z+1)/bSize; ++z){
                //     for (int y = 0; y < (hi.y-lo.y+1)/bSize; ++y){
                //         for (int x = 0; x < (hi.x-lo.x+1)/bSize; ++x){
                //             // todo bb = 0
                //             for (int k = lo.z+z*bSize; k < lo.z+z*bSize+bSize; ++k){
                //                 for (int j =lo.y+y*bSize; j <lo.y+y*bSize+bSize; ++j){
                //                     for (int i = lo.x+x*bSize; i <lo.x+x*bSize+bSize; ++i){
                //                         if(fab_array(i,j,k,0) != bogus_flag) {
                //                             cc = cnt/(unitBlkSize);
                //                             zz = cc/big2X;
                //                             yy = (cc - zz*big2X)/bigX;
                //                             xx = cc -zz*big2X - yy*bigX;

                //                             bb = cnt - unitBlkSize*cc;

                //                             kk = bb/b2Size;
                //                             jjj = (bb - kk*b2Size)/bSize;
                //                             ii = bb - kk*b2Size - jjj*bSize;
                //                             // if (b_buffer[(xx*bSize+ii) + (yy*bSize+jjj)*bSize*bigX + (zz*bSize+kk)*big2X*b2Size] != 0)
                //                             //     std::cout << b_buffer[(xx*bSize+ii) + (yy*bSize+jjj)*bSize*bigX + (zz*bSize+kk)*big2X*b2Size] <<std::endl;
                //                             b_buffer[(xx*bSize+ii) + (yy*bSize+jjj)*bSize*bigX + (zz*bSize+kk)*big2X*b2Size] = fab_array(i,j,k,jj);

                //                             cnt++;
                //                         }
                //                     }
                //                 }
                //             }
                //         }
                //     }
                // }

                /*nast*/
                for (int z = 0; z < (hi.z-lo.z+1)/bSize; ++z)
                    for (int y = 0; y < (hi.y-lo.y+1)/bSize; ++y)
                        for (int x = 0; x < (hi.x-lo.x+1)/bSize; ++x)
                            for (int k = lo.z+z*bSize; k < lo.z+z*bSize+bSize; ++k)
                                for (int j =lo.y+y*bSize; j <lo.y+y*bSize+bSize; ++j)
                                    for (int i = lo.x+x*bSize; i <lo.x+x*bSize+bSize; ++i){
                                        if(fab_array(i,j,k,0) != bogus_flag) {
                                            b_buffer[cnt] = fab_array(i,j,k,jj);
                                            cnt++;
                                        }
                                        // /*bs*/
                                        // else {
                                        //     b_buffer[cnt] = 0;
                                        //     cnt++;
                                        // }
                                    }


                /*1d*/
                // for (int z = lo.z; z <= hi.z; ++z)
                //     for (int y = lo.y; y <= hi.y; ++y)
                //         for (int x = lo.x; x <= hi.x; ++x) {
                //             if(fab_array(x,y,z,0) != bogus_flag) {
                //                 b_buffer[cnt] = fab_array(x,y,z,jj);
                //                 cnt++;
                //             }
                //             /*bs*/
                //             else {
                //                 cnt++;
                //             }
                //         }

            }
            if (jj == 0) {
                /*stack*/
                // size_t bigZ = ((cnt/(bSize*bSize*bSize)) + (bigX*bigX) - 1) / (bigX*bigX);
                // std::cout << "bigZ: " << bigZ << std::endl;

                // cnt = bigZ*bigX*bigX*bSize*bSize*bSize;
                // std::cout << "fill cnt in proc: " << myProc << " : "<< cnt << std::endl;
                // sprintf(meta_name, "meta/s_%d_%d.txt", level, myProc);
                // std::ofstream sfile(meta_name);
                // if (!sfile.is_open()) {
                //     std::cout << "Error opening metadata file\n";
                //     return;
                // }
                // sfile << bigX;
                // sfile.close();

                char meta_name[64];
                 std::cout<< "cnt in buffer " << myProc << " : " << cnt << std::endl;

                if(ParallelDescriptor::IOProcessor()) {
                    sprintf(meta_name, "meta/sp_%d.txt", level);
                    std::ofstream spfile(meta_name);
                    if (!spfile.is_open()) {
                        std::cout << "Error opening metadata file\n";
                        return;
                    }
                    for (auto i = sortedProcs.begin(); i != sortedProcs.end(); ++i)
                        spfile << *i << " ";
                    spfile.close();
                }

                realProcBufferSize[myProc] = cnt;

                if (level == finest_level) {
                    if(ParallelDescriptor::IOProcessor()) {
                        char meta_name[64];
                        sprintf(meta_name, "meta/meta_%d.txt", level);
                        std::ofstream outfile(meta_name);
                        if (!outfile.is_open()) {
                            std::cout << "Error opening metadata file\n";
                            return;
                        }
                        for(int i(0); i < nProcs; i++)
                            outfile <<  realProcBufferSize[i] << std::endl;
                        outfile.close();

                        sprintf(meta_name, "meta/f.txt", level);
                        std::ofstream ffile(meta_name);
                        if (!ffile.is_open()) {
                            std::cout << "Error opening metadata file\n";
                            return;
                        }
                        ffile << level;
                        ffile.close();

                        sprintf(meta_name, "meta/p.txt", level);
                        std::ofstream pfile(meta_name);
                        if (!pfile.is_open()) {
                            std::cout << "Error opening metadata file\n";
                            return;
                        }
                        pfile << nProcs;
                        pfile.close();

                    }
                } else {
                    char meta_name[128];
                    sprintf(meta_name, "meta/meta_%d_%d.txt", level, myProc);
                    std::ofstream outfile(meta_name);
                    if (!outfile.is_open()) {
                        std::cout << "Error opening metadata file\n";
                        return;
                    }
                    outfile <<  realProcBufferSize[myProc];
                    outfile.close();
                }
            }

        double eb = 0.001;
        std::ifstream inputFile("eb.txt");
        if (inputFile.is_open()) {
            inputFile >> eb;
            inputFile.close();
        } else {
            std::cerr << "Unable to open the eb file." << std::endl;
        }

        dcpl_id_lev = H5Pcopy(dcpl_id);
#ifdef AMREX_USE_HDF5_SZ
        if (mode_env == "SZ") {
            size_t cd_nelmts;
            unsigned int* cd_values = NULL;
            unsigned filter_config;
            SZ_errConfigToCdArray(&cd_nelmts, &cd_values, 1, 0, eb, level, realProcBufferSize[myProc]);
            H5Pset_filter(dcpl_id_lev, H5Z_FILTER_SZ, H5Z_FLAG_MANDATORY, cd_nelmts, cd_values);
        }
#endif

#ifdef AMREX_USE_HDF5_SZ3
        if (mode_env == "SZ") {
            size_t cd_nelmts;
            unsigned int* cd_values = NULL;
            unsigned filter_config;
            SZ_errConfigToCdArray(&cd_nelmts, &cd_values, 1, 0, eb, level, realProcBufferSize[myProc]);
            H5Pset_filter(dcpl_id_lev, H5Z_FILTER_SZ3, H5Z_FLAG_MANDATORY, cd_nelmts, cd_values);
        }
#endif

            sprintf(dataname, "data:datatype=%d", jj);
#ifdef AMREX_USE_HDF5_ASYNC
            dataset = H5Dcreate_async(grp, dataname, H5T_NATIVE_DOUBLE, dataspace, H5P_DEFAULT, dcpl_id_lev, H5P_DEFAULT, es_id_g);
            if(dataset < 0) std::cout << ParallelDescriptor::MyProc() << "create data failed!  ret = " << dataset << std::endl;
            ret = H5Dwrite_async(dataset, H5T_NATIVE_DOUBLE, memdataspace, dataspace, dxpl_col, b_buffer.dataPtr(), es_id_g);
            if(ret < 0) { std::cout << ParallelDescriptor::MyProc() << "Write data failed!  ret = " << ret << std::endl; break; }
            H5Dclose_async(dataset, es_id_g);
#else
            dataset = H5Dcreate(grp, dataname, H5T_NATIVE_DOUBLE, dataspace, H5P_DEFAULT, dcpl_id_lev, H5P_DEFAULT);
            if(dataset < 0) std::cout << ParallelDescriptor::MyProc() << "create data failed!  ret = " << dataset << std::endl;
            ret = H5Dwrite(dataset, H5T_NATIVE_DOUBLE, memdataspace, dataspace, dxpl_col, b_buffer.dataPtr());
            if(ret < 0) { std::cout << ParallelDescriptor::MyProc() << "Write data failed!  ret = " << ret << std::endl; break; }
            H5Dclose(dataset);
#endif
        }

        BL_PROFILE_VAR_STOP(h5dwg);
        H5Pclose(dcpl_id_lev);
        H5Sclose(memdataspace);
        H5Sclose(dataspace);
        H5Sclose(offsetdataspace);
        H5Sclose(centerdataspace);
        H5Sclose(boxdataspace);

#ifdef AMREX_USE_HDF5_ASYNC
        H5Dclose_async(offsetdataset, es_id_g);
        H5Dclose_async(centerdataset, es_id_g);
        H5Dclose_async(boxdataset, es_id_g);
        H5Gclose_async(grp, es_id_g);
#else
        H5Dclose(offsetdataset);
        H5Dclose(centerdataset);
        H5Dclose(boxdataset);
        H5Gclose(grp);
#endif
    } // For group

    BL_PROFILE_VAR_STOP(h5dwd);

    H5Tclose(center_id);
    H5Tclose(babox_id);
    H5Pclose(fapl);
    H5Pclose(dcpl_id);
    H5Pclose(dxpl_col);
    H5Pclose(dxpl_ind);

#ifdef AMREX_USE_HDF5_ASYNC
    H5Fclose_async(fid, es_id_g);
#else
    H5Fclose(fid);
#endif


} // WriteMultiLevelPlotfileHDF5MultiDset

void
WriteSingleLevelPlotfileHDF5 (const std::string& plotfilename,
                              const MultiFab& mf, const Vector<std::string>& varnames,
                              const Geometry& geom, Real time, int level_step,
                              const std::string &compression,
                              const std::string &versionName,
                              const std::string &levelPrefix,
                              const std::string &mfPrefix,
                              const Vector<std::string>& extra_dirs)
{
    Vector<const MultiFab*> mfarr(1,&mf);
    Vector<Geometry> geomarr(1,geom);
    Vector<int> level_steps(1,level_step);
    Vector<IntVect> ref_ratio;

    WriteMultiLevelPlotfileHDF5(plotfilename, 1, mfarr, varnames, geomarr, time, level_steps, ref_ratio,
                                compression, versionName, levelPrefix, mfPrefix, extra_dirs);
}

void
WriteSingleLevelPlotfileHDF5SingleDset (const std::string& plotfilename,
                                        const MultiFab& mf, const Vector<std::string>& varnames,
                                        const Geometry& geom, Real time, int level_step,
                                        const std::string &compression,
                                        const std::string &versionName,
                                        const std::string &levelPrefix,
                                        const std::string &mfPrefix,
                                        const Vector<std::string>& extra_dirs)
{
    Vector<const MultiFab*> mfarr(1,&mf);
    Vector<Geometry> geomarr(1,geom);
    Vector<int> level_steps(1,level_step);
    Vector<IntVect> ref_ratio;

    WriteMultiLevelPlotfileHDF5SingleDset(plotfilename, 1, mfarr, varnames, geomarr, time, level_steps, ref_ratio,
                                          compression, versionName, levelPrefix, mfPrefix, extra_dirs);
}

void
WriteSingleLevelPlotfileHDF5MultiDset (const std::string& plotfilename,
                                       const MultiFab& mf, const Vector<std::string>& varnames,
                                       const Geometry& geom, Real time, int level_step,
                                       const std::string &compression,
                                       const std::string &versionName,
                                       const std::string &levelPrefix,
                                       const std::string &mfPrefix,
                                       const Vector<std::string>& extra_dirs)
{
    Vector<const MultiFab*> mfarr(1,&mf);
    Vector<Geometry> geomarr(1,geom);
    Vector<int> level_steps(1,level_step);
    Vector<IntVect> ref_ratio;

    WriteMultiLevelPlotfileHDF5MultiDset(plotfilename, 1, mfarr, varnames, geomarr, time, level_steps, ref_ratio,
                                         compression, versionName, levelPrefix, mfPrefix, extra_dirs);
}

void
WriteMultiLevelPlotfileHDF5 (const std::string &plotfilename,
                             int nlevels,
                             const Vector<const MultiFab*> &mf,
                             const Vector<std::string> &varnames,
                             const Vector<Geometry> &geom,
                             Real time,
                             const Vector<int> &level_steps,
                             const Vector<IntVect> &ref_ratio,
                             const std::string &compression,
                             const std::string &versionName,
                             const std::string &levelPrefix,
                             const std::string &mfPrefix,
                             const Vector<std::string>& extra_dirs)
{

    WriteMultiLevelPlotfileHDF5SingleDset(plotfilename, nlevels, mf, varnames, geom, time, level_steps, ref_ratio,
                                          compression, versionName, levelPrefix, mfPrefix, extra_dirs);
}

} // namespace amrex

