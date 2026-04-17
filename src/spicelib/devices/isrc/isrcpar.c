/**********
Copyright 1990 Regents of the University of California.  All rights reserved.
Author: 1985 Thomas L. Quarles
Modified: 2000 AlansFixes
**********/
/*
 */

#include "ngspice/ngspice.h"
#include "ngspice/ifsim.h"
#include "isrcdefs.h"
#include "ngspice/sperror.h"
#include "ngspice/suffix.h"
#include "ngspice/1-f-code.h"


static void copy_coeffs(ISRCinstance *here, IFvalue *value)
{
    int n = value->v.numValue;

    if(here->ISRCcoeffs)
        tfree(here->ISRCcoeffs);

    here->ISRCcoeffs = TMALLOC(double, n);
    here->ISRCfunctionOrder = n;
    here->ISRCcoeffsGiven = TRUE;

    memcpy(here->ISRCcoeffs, value->v.vec.rVec, (size_t) n * sizeof(double));
}


static double pwl_atof(const char *s, char **endptr) {
    double val = strtod(s, endptr);
    if (*endptr && **endptr != '\0' && !isspace_c(**endptr)) {
        char suffix = tolower_c(**endptr);
        double factor = 1.0;
        switch (suffix) {
            case 't': factor = 1e12; break;
            case 'g': factor = 1e9; break;
            case 'k': factor = 1e3; break;
            case 'm': 
                if (tolower_c((*endptr)[1]) == 'e' && tolower_c((*endptr)[2]) == 'g') {
                    factor = 1e6; (*endptr) += 2;
                } else {
                    factor = 1e-3;
                }
                break;
            case 'u': factor = 1e-6; break;
            case 'n': factor = 1e-9; break;
            case 'p': factor = 1e-12; break;
            case 'f': factor = 1e-15; break;
            case 'a': factor = 1e-18; break;
        }
        val *= factor;
        (*endptr)++;
    }
    return val;
}

static int load_pwl_file(const char *filename, double **coeffs, int *count) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: Could not open PWL file '%s'\n", filename);
        return (E_NOTFOUND);
    }
    double *c = NULL;
    int n = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *end;
        while (*p && isspace_c(*p)) p++;
        if (!*p || *p == '*' || *p == ';' || *p == '#') continue;

        double t = pwl_atof(p, &end);
        if (p == end) continue;
        p = end;
        while (*p && (isspace_c(*p) || *p == ',')) p++;
        
        double v = pwl_atof(p, &end);
        if (p == end) continue;

        c = TREALLOC(double, c, n + 2);
        c[n++] = t;
        c[n++] = v;
    }
    fclose(fp);
    if (n < 2) {
        fprintf(stderr, "Error: PWL file '%s' is empty or invalid\n", filename);
        if (c) tfree(c);
        return (E_PARMVAL);
    }
    *coeffs = c;
    *count = n;
    return (OK);
}


/* ARGSUSED */
int
ISRCparam(int param, IFvalue *value, GENinstance *inst, IFvalue *select)
{
    int i;
    ISRCinstance *here = (ISRCinstance *) inst;

    NG_IGNORE(select);

    switch (param) {

        case ISRC_DC:
            here->ISRCdcValue = value->rValue;
            here->ISRCdcGiven = TRUE;
            break;

        case ISRC_M:
            here->ISRCmValue = value->rValue;
            here->ISRCmGiven = TRUE;
            break;

        case ISRC_AC_MAG:
            here->ISRCacMag = value->rValue;
            here->ISRCacMGiven = TRUE;
            here->ISRCacGiven = TRUE;
            break;

        case ISRC_AC_PHASE:
            here->ISRCacPhase = value->rValue;
            here->ISRCacPGiven = TRUE;
            here->ISRCacGiven = TRUE;
            break;

        case ISRC_AC:
            /* FALLTHROUGH added to suppress GCC warning due to
             * -Wimplicit-fallthrough flag */
            switch (value->v.numValue) {
                case 2:
                    here->ISRCacPhase = *(value->v.vec.rVec+1);
                    here->ISRCacPGiven = TRUE;
                    /* FALLTHROUGH */
                case 1:
                    here->ISRCacMag = *(value->v.vec.rVec);
                    here->ISRCacMGiven = TRUE;
                    /* FALLTHROUGH */
                case 0:
                    here->ISRCacGiven = TRUE;
                    break;
                default:
                    return(E_BADPARM);
            }
            break;

        case ISRC_PULSE:
            if(value->v.numValue < 2)
                return(E_BADPARM);
            here->ISRCfunctionType = PULSE;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);
            break;

        case ISRC_SINE:
            if(value->v.numValue < 2)
                return(E_BADPARM);
            here->ISRCfunctionType = SINE;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);
            break;

        case ISRC_EXP:
            if(value->v.numValue < 2)
                return(E_BADPARM);
            here->ISRCfunctionType = EXP;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);
            break;

        case ISRC_PWL:
            if(value->v.numValue < 2)
                return(E_BADPARM);
            here->ISRCfunctionType = PWL;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);

            for (i=0; i<(here->ISRCfunctionOrder/2)-1; i++) {
                  if (*(here->ISRCcoeffs+2*(i+1))<=*(here->ISRCcoeffs+2*i)) {
                     fprintf(stderr, "Warning : current source %s",
                                                               here->ISRCname);
                     fprintf(stderr, " has non-increasing PWL time points.\n");
                  }
            }

            break;

        case ISRC_SFFM:
            if(value->v.numValue < 2)
                return(E_BADPARM);
            here->ISRCfunctionType = SFFM;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);
            break;

        case ISRC_AM:
            if(value->v.numValue < 2)
                return(E_BADPARM);
            here->ISRCfunctionType = AM;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);
            break;

        case ISRC_D_F1:
            here->ISRCdF1given = TRUE;
            here->ISRCdGiven = TRUE;
            switch(value->v.numValue) {
            case 2:
                here->ISRCdF1phase = *(value->v.vec.rVec+1);
                here->ISRCdF1mag = *(value->v.vec.rVec);
                break;
            case 1:
                here->ISRCdF1mag = *(value->v.vec.rVec);
                here->ISRCdF1phase = 0.0;
                break;
            case 0:
                here->ISRCdF1mag = 1.0;
                here->ISRCdF1phase = 0.0;
                break;
            default:
                return(E_BADPARM);
            }
            break;

        case ISRC_D_F2:
            here->ISRCdF2given = TRUE;
            here->ISRCdGiven = TRUE;
            switch(value->v.numValue) {
            case 2:
                here->ISRCdF2phase = *(value->v.vec.rVec+1);
                here->ISRCdF2mag = *(value->v.vec.rVec);
                break;
            case 1:
                here->ISRCdF2mag = *(value->v.vec.rVec);
                here->ISRCdF2phase = 0.0;
                break;
            case 0:
                here->ISRCdF2mag = 1.0;
                here->ISRCdF2phase = 0.0;
                break;
            default:
                return(E_BADPARM);
            }
            break;

        case ISRC_TRNOISE: {
            double NA, TS;
            double NALPHA = 0.0;
            double NAMP   = 0.0;
            double RTSAM   = 0.0;
            double RTSCAPT   = 0.0;
            double RTSEMT   = 0.0;

            here->ISRCfunctionType = TRNOISE;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);

            NA = here->ISRCcoeffs[0]; // input is rms value
            TS = here->ISRCcoeffs[1]; // time step

            if (here->ISRCfunctionOrder > 2)
                NALPHA = here->ISRCcoeffs[2]; // 1/f exponent

            if (here->ISRCfunctionOrder > 3 && NALPHA != 0.0)
                NAMP = here->ISRCcoeffs[3]; // 1/f amplitude

            if (here->ISRCfunctionOrder > 4)
                RTSAM = here->ISRCcoeffs[4]; // RTS amplitude

            if (here->ISRCfunctionOrder > 5 && RTSAM != 0.0)
                RTSCAPT = here->ISRCcoeffs[5]; // RTS trap capture time

            if (here->ISRCfunctionOrder > 6 && RTSAM != 0.0)
                RTSEMT = here->ISRCcoeffs[6]; // RTS trap emission time
            /* after an 'alter' command to the TRNOISE voltage source the state gets re-written
               with the new parameters. So free the old state first. */
            trnoise_state_free(here->ISRCtrnoise_state);
            here->ISRCtrnoise_state =
                trnoise_state_init(NA, TS, NALPHA, NAMP, RTSAM, RTSCAPT, RTSEMT);
        }
        break;

        case ISRC_TRRANDOM: {
            double TD = 0.0, TS;
            int rndtype = 1;
            double PARAM1 = 1.0;
            double PARAM2 = 0.0;

            here->ISRCfunctionType = TRRANDOM;
            here->ISRCfuncTGiven = TRUE;
            copy_coeffs(here, value);

            rndtype = (int)here->ISRCcoeffs[0]; // type of random function
            TS = here->ISRCcoeffs[1]; // time step
            if (here->ISRCfunctionOrder > 2)
                TD = here->ISRCcoeffs[2]; // delay

            if (here->ISRCfunctionOrder > 3)
                PARAM1 = here->ISRCcoeffs[3]; // first parameter

            if (here->ISRCfunctionOrder > 4)
                PARAM2 = here->ISRCcoeffs[4]; // second parameter

            /* after an 'alter' command to the TRRANDOM voltage source the state gets re-written
               with the new parameters. So free the old state first. */
            tfree(here->ISRCtrrandom_state);
            here->ISRCtrrandom_state =
                trrandom_state_init(rndtype, TS, TD, PARAM1, PARAM2);
        }
        break;

#ifdef SHARED_MODULE
        case ISRC_EXTERNAL: {
            here->ISRCfunctionType = EXTERNAL;
            here->ISRCfuncTGiven = TRUE;
            /* no coefficients
            copy_coeffs(here, value);
            */
        }
        break;
#endif

        case ISRC_WAVEFILE:
            if (here->ISRCwaveFile) tfree(here->ISRCwaveFile);
            here->ISRCwaveFile = strdup(value->sValue);
            here->ISRCwaveFileGiven = TRUE;
            here->ISRCfunctionType = WAVE;
            here->ISRCfuncTGiven = TRUE;
            
            /* Attempt initial load */
            here->ISRCwavPtr = (VSRCwavData *)VSRCwavLoad(here->ISRCwaveFile);

            /* Case-insensitive path fallback */
            if (!here->ISRCwavPtr) {
                char *tmp = strdup(here->ISRCwaveFile);
                if (strstr(tmp, "/music/")) {
                    char *p = strstr(tmp, "/music/");
                    p[1] = 'M';
                    here->ISRCwavPtr = (VSRCwavData *)VSRCwavLoad(tmp);
                }
                free(tmp);
            }
            break;

        case ISRC_WAVECHAN:
            here->ISRCwaveChan = value->iValue;
            here->ISRCwaveChanGiven = TRUE;
            break;

        case ISRC_PWL_FILE: {
            double *coeffs = NULL;
            int count = 0;
            int err = load_pwl_file(value->sValue, &coeffs, &count);
            if (err != OK) return (err);

            if (here->ISRCcoeffs) tfree(here->ISRCcoeffs);
            here->ISRCcoeffs = coeffs;
            here->ISRCfunctionOrder = count;
            here->ISRCcoeffsGiven = TRUE;
            here->ISRCfunctionType = PWL;
            here->ISRCfuncTGiven = TRUE;

            for (i=0; i<(here->ISRCfunctionOrder/2)-1; i++) {
                  if (*(here->ISRCcoeffs+2*(i+1))<=*(here->ISRCcoeffs+2*i)) {
                     fprintf(stderr, "Warning : current source %s has non-increasing PWL time points in file '%s'.\n", 
                             here->ISRCname, value->sValue);
                  }
            }
            break;
        }

        default:
            return(E_BADPARM);
    }

    return(OK);
}
