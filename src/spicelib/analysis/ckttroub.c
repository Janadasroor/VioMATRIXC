/**********
Copyright 1990 Regents of the University of California.  All rights reserved.
**********/

#include "ngspice/ngspice.h"
#include "ngspice/trandefs.h"
#include "ngspice/cktdefs.h"
#include "ngspice/devdefs.h"
#include "vsrc/vsrcdefs.h"
#include "isrc/isrcdefs.h"
#include "res/resdefs.h"
#include "ngspice/jobdefs.h"

#include "analysis.h"


extern SPICEanalysis *analInfo[];

char *
CKTtrouble(CKTcircuit *ckt, char *optmsg)
{
    char	msg_buf[513];
    size_t	rem = sizeof(msg_buf);
    char	*emsg;
    TRCV	*cv;
    int		vcode, icode, rcode;
    char	*msg_p;
    SPICEanalysis *an;
    int		i;
    size_t	n;

    if (!ckt || !ckt->CKTcurJob)
	return NULL;

    an = analInfo[ckt->CKTcurJob->JOBtype];

    int sn;
    if (optmsg && *optmsg) {
       sn = snprintf(msg_buf, rem, "%s:  %s; ", an->if_analysis.name, optmsg);
    } else {
       sn = snprintf(msg_buf, rem, "%s:  ", an->if_analysis.name);
    }
    if (sn > 0 && (size_t)sn < rem) { n = (size_t)sn; rem -= n; } else rem = 0;

    msg_p = msg_buf + sizeof(msg_buf) - rem;

    switch (an->domain) {
    case TIMEDOMAIN:
	if (ckt->CKTtime == 0.0)
	    sn = snprintf(msg_p, rem, "initial timepoint: ");
	else
	    sn = snprintf(msg_p, rem, "time = %g, timestep = %g: ", ckt->CKTtime,
		ckt->CKTdelta);
	if (sn > 0 && (size_t)sn < rem) { n = (size_t)sn; rem -= n; msg_p = msg_buf + sizeof(msg_buf) - rem; } else rem = 0;
	break;

    case FREQUENCYDOMAIN:
	sn = snprintf(msg_p, rem, "frequency = %g: ", ckt->CKTomega / (2.0 * M_PI));
	if (sn > 0 && (size_t)sn < rem) { n = (size_t)sn; rem -= n; msg_p = msg_buf + sizeof(msg_buf) - rem; } else rem = 0;
	break;

    case SWEEPDOMAIN:
	cv = (TRCV*) ckt->CKTcurJob;
	vcode = CKTtypelook("Vsource");
	icode = CKTtypelook("Isource");
	rcode = CKTtypelook("Resistor");

	for (i = 0; i <= cv->TRCVnestLevel; i++) {
		if (cv->TRCVvType[i] == vcode) { /* voltage source */
			sn = snprintf(msg_p, rem, " %s = %g: ", cv->TRCVvName[i],
				((VSRCinstance*)(cv->TRCVvElt[i]))->VSRCdcValue);
		}
		else if (cv->TRCVvType[i] == TEMP_CODE) { /* temp sweep, if optran fails) */
			sn = snprintf(msg_p, rem, " %s = %g: ", cv->TRCVvName[i], ckt->CKTtemp - CONSTCtoK);
		}
		else if (cv->TRCVvType[i] == rcode) {
			sn = snprintf(msg_p, rem, " %s = %g: ", cv->TRCVvName[i],
				((RESinstance*)(cv->TRCVvElt[i]))->RESresist);
	    } else {
		sn = snprintf(msg_p, rem, " %s = %g: ", cv->TRCVvName[i],
		    ((ISRCinstance*)(cv->TRCVvElt[i]))->ISRCdcValue);
	    }
	    if (sn > 0 && (size_t)sn < rem) { n = (size_t)sn; rem -= n; msg_p = msg_buf + sizeof(msg_buf) - rem; } else rem = 0;
	}
	break;

    case NODOMAIN:
    default:
	break;
    }

    if (ckt->CKTtroubleNode) {
	sn = snprintf(msg_p, rem, "trouble with node \"%s\"\n",
		CKTnodName(ckt, ckt->CKTtroubleNode));
    } else if (ckt->CKTtroubleElt) {
	/* "-" for dop */
	sn = snprintf(msg_p, rem, "trouble with %s-instance %s\n",
	    ckt->CKTtroubleElt->GENmodPtr->GENmodName,
	    ckt->CKTtroubleElt->GENname);
    } else {
	sn = snprintf(msg_p, rem, "cause unrecorded.\n");
    }

    emsg = TMALLOC(char, strlen(msg_buf) + 1);
    strcpy(emsg,msg_buf);

    return emsg;
}
