/* The MIT License

   Copyright (c) 2013-2014 Genome Research Ltd.
   Authors:  see http://github.com/samtools/bcftools/blob/master/AUTHORS

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.

 */

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <math.h>
#include <htslib/vcf.h>
#include <htslib/synced_bcf_reader.h>
#include <htslib/kstring.h>
#include <htslib/kseq.h>
#include "bcftools.h"
#include "HMM.h"

#define STATE_HW 0        // normal state, follows Hardy-Weinberg allele frequencies
#define STATE_AZ 1        // autozygous state

/** Genetic map */
typedef struct
{
    int pos;
    double rate;
}
genmap_t;

typedef struct _args_t
{
    bcf_srs_t *files;
    bcf_hdr_t *hdr;
    double t2AZ, t2HW;      // P(AZ|HW) and P(HW|AZ) parameters
    double unseen_PL;

    char *genmap_fname;
    genmap_t *genmap;
    int ngenmap, mgenmap, igenmap;

    hmm_t *hmm;
    double *eprob;          // emission probs [2*nsites,msites]
    uint32_t *sites;        // positions [nsites,msites]
    int nsites, msites;

    int32_t *itmp;
    int nitmp, mitmp;
    float *AFs;
    int mAFs;

    double pl2p[256], *pdg;
    int32_t skip_rid, prev_rid, prev_pos;

    int ntot, nused;            // some stats to detect if things didn't go awfully wrong
    int ismpl, nsmpl;           // index of query sample
    char *estimate_AF, *sample; // list of samples for AF estimate and query sample
    char **argv, *targets_list, *regions_list, *af_fname, *af_tag;
    int argc, fake_PLs, snps_only, vi_training;
}
args_t;

void set_tprob(hmm_t *hmm, uint32_t prev_pos, uint32_t pos, void *data);

void *smalloc(size_t size)
{
    void *mem = malloc(size);
    if ( !mem ) error("malloc: Could not allocate %d bytes\n", (int)size);
    return mem;
}

static void init_data(args_t *args)
{
    args->prev_rid = args->skip_rid = -1;
    args->hdr = args->files->readers[0].header;

    // Set samples
    kstring_t str = {0,0,0};
    if ( args->estimate_AF && strcmp("-",args->estimate_AF) )
    {
        int i, n;
        char **smpls = hts_readlist(args->estimate_AF, 1, &n);

        // Make sure the query sample is included
        for (i=0; i<n; i++)
            if ( !strcmp(args->sample,smpls[i]) ) break;

        // Add the query sample if not present
        if ( i!=n ) kputs(args->sample, &str);

        for (i=0; i<n; i++)
        {
            if ( str.l ) kputc(',', &str);
            kputs(smpls[i], &str);
            free(smpls[i]);
        }
        free(smpls);
    }
    else
        kputs(args->sample, &str);

    int ret = bcf_hdr_set_samples(args->hdr, str.s, 0);
    if ( ret<0 ) error("Error parsing the list of samples: %s\n", str.s);
    else if ( ret>0 ) error("The %d-th sample not found in the VCF\n", ret);

    if ( args->af_tag )
        if ( !bcf_hdr_idinfo_exists(args->hdr,BCF_HL_INFO,bcf_hdr_id2int(args->hdr,BCF_DT_ID,args->af_tag)) ) 
            error("No such INFO tag in the VCF: %s\n", args->af_tag);

    if ( !bcf_sr_set_samples(args->files, str.s, 0) )
        error("Error: could not set the samples %s\n", str.s);
    args->nsmpl = args->files->n_smpl;
    args->ismpl = bcf_hdr_id2int(args->hdr, BCF_DT_SAMPLE, args->sample);
    free(str.s);

    int i;
    for (i=0; i<256; i++) args->pl2p[i] = pow(10., -i/10.);

    // Init transition matrix and HMM
    double tprob[4];
    MAT(tprob,2,STATE_HW,STATE_HW) = 1 - args->t2AZ;
    MAT(tprob,2,STATE_HW,STATE_AZ) = args->t2HW;
    MAT(tprob,2,STATE_AZ,STATE_HW) = args->t2AZ;
    MAT(tprob,2,STATE_AZ,STATE_AZ) = 1 - args->t2HW; 

    if ( args->genmap_fname ) 
    {
        args->hmm = hmm_init(2, tprob, 0);
        hmm_set_tprob_func(args->hmm, set_tprob, args);
    }
    else
        args->hmm = hmm_init(2, tprob, 10000);

    // print header
    printf("# This file was produced by: bcftools roh(%s+htslib-%s)\n", bcftools_version(),hts_version());
    printf("# The command line was:\tbcftools %s", args->argv[0]);
    for (i=1; i<args->argc; i++)
        printf(" %s",args->argv[i]);
    printf("\n#\n");
    printf("# [1]Chromosome\t[2]Position\t[3]State (0:HW, 1:AZ)\t[4]Quality\n");
}

static void destroy_data(args_t *args)
{
    hmm_destroy(args->hmm);
    bcf_sr_destroy(args->files);
    free(args->itmp); free(args->AFs); free(args->pdg);
    free(args->genmap);
}

static int load_genmap(args_t *args, bcf1_t *line)
{
    if ( !args->genmap_fname ) { args->ngenmap = 0; return 0; }

    kstring_t str = {0,0,0};
    char *fname = strstr(args->genmap_fname,"{CHROM}");
    if ( fname )
    {
        kputsn(args->genmap_fname, fname - args->genmap_fname, &str);
        kputs(bcf_seqname(args->hdr,line), &str);
        kputs(fname+7,&str);
        fname = str.s;
    }
    else
        fname = args->genmap_fname;

    htsFile *fp = hts_open(fname, "rb");
    if ( !fp ) 
    {
        args->ngenmap = 0;
        return -1;
    }

    hts_getline(fp, KS_SEP_LINE, &str);
    if ( strcmp(str.s,"position COMBINED_rate(cM/Mb) Genetic_Map(cM)") ) 
        error("Unexpected header, found:\n\t[%s], but expected:\n\t[position COMBINED_rate(cM/Mb) Genetic_Map(cM)]\n", fname, str.s);

    args->ngenmap = args->igenmap = 0;
    while ( hts_getline(fp, KS_SEP_LINE, &str) > 0 )
    {
        args->ngenmap++;
        hts_expand(genmap_t,args->ngenmap,args->mgenmap,args->genmap);
        genmap_t *gm = &args->genmap[args->ngenmap-1];

        char *tmp, *end;
        gm->pos = strtol(str.s, &tmp, 10);
        if ( str.s==tmp ) error("Could not parse %s: %s\n", fname, str.s);

        // skip second column
        tmp++;
        while ( *tmp && !isspace(*tmp) ) tmp++;
        
        // read the genetic map in cM
        gm->rate = strtod(tmp+1, &end);
        if ( tmp+1==end ) error("Could not parse %s: %s\n", fname, str.s);
    }
    if ( !args->ngenmap ) error("Genetic map empty?\n");
    int i;
    for (i=0; i<args->ngenmap; i++) args->genmap[i].rate /= args->genmap[args->ngenmap-1].rate; // scale to 1
    if ( hts_close(fp) ) error("Close failed\n");
    free(str.s);
    return 0;
}

static double get_genmap_rate(args_t *args, int start, int end)
{
    // position i to be equal to or smaller than start
    int i = args->igenmap;
    if ( args->genmap[i].pos > start )
    {
        while ( i>0 && args->genmap[i].pos > start ) i--;
    }
    else
    {
        while ( i+1<args->ngenmap && args->genmap[i+1].pos < start ) i++;
    }
    // position j to be equal or larger than end
    int j = i;
    while ( j+1<args->ngenmap && args->genmap[j].pos < end ) j++;

    if ( i==j ) 
    {
        args->igenmap = i;
        return 0;
    }

    if ( start <  args->genmap[i].pos ) start = args->genmap[i].pos;
    if ( end >  args->genmap[j].pos ) end = args->genmap[j].pos;
    double rate = (args->genmap[j].rate - args->genmap[i].rate)/(args->genmap[j].pos - args->genmap[i].pos) * (end-start);
    args->igenmap = j;
    return rate;
}

void set_tprob(hmm_t *hmm, uint32_t prev_pos, uint32_t pos, void *data)
{
    args_t *args = (args_t*) data;
    double ci = get_genmap_rate(args, pos - prev_pos, pos);
    MAT(hmm->tprob,2,STATE_HW,STATE_HW) *= 1-ci;
    MAT(hmm->tprob,2,STATE_HW,STATE_AZ) *= ci;
    MAT(hmm->tprob,2,STATE_AZ,STATE_HW) *= ci;
    MAT(hmm->tprob,2,STATE_AZ,STATE_AZ) *= 1-ci;
}

void update_tprobs(hmm_t *hmm)
{
    memset(hmm->tprob,0,sizeof(*hmm->tprob)*hmm->nstates*hmm->nstates); 

    int i, j;
    for (i=1; i<hmm->nsites; i++)
    {
        int prev_state = hmm->vpath[hmm->nstates*(i-1)];
        int curr_state = hmm->vpath[hmm->nstates*i];
        MAT(hmm->tprob,hmm->nstates,curr_state,prev_state) += 1;
    }
    for (i=0; i<hmm->nstates; i++)
    {
        int n = 0;
        for (j=0; j<hmm->nstates; j++) n += MAT(hmm->tprob,hmm->nstates,i,j);
        assert( n );
        for (j=0; j<hmm->nstates; j++) MAT(hmm->tprob,hmm->nstates,i,j) /= n;
    }
    hmm_set_tprob(hmm, hmm->tprob, 10000);
}


/**
 *  This function implements the HMM model:
 *    D = Data, AZ = autozygosity, HW = Hardy-Weinberg (non-autozygosity), 
 *    f = non-ref allele frequency
 * 
 *  Emission probabilities:
 *    oAZ = P_i(D|AZ) = (1-f)*P(D|RR) + f*P(D|AA)
 *    oHW = P_i(D|HW) = (1-f)^2 * P(D|RR) + f^2 * P(D|AA) + 2*f*(1-f)*P(D|RA)
 *
 *  Transition probabilities:
 *    tAZ = P(AZ|HW)  .. parameter
 *    tHW = P(HW|AZ)  .. parameter
 *    P(AZ|AZ) = 1 - P(HW|AZ) = 1 - tHW
 *    P(HW|HW) = 1 - P(AZ|HW) = 1 - tAZ
 *
 *    ci  = P_i(C)    .. probability of cross-over at site i, from genetic map
 *
 *    AZi = P_i(AZ)   .. probability of site i being AZ/non-AZ, scaled so that AZi+HWi = 1
 *    HWi = P_i(HW) 
 *
 *    P_i(AZ|AZ) = P(AZ|AZ) * (1-ci) * AZ{i-1} = (1-tHW) * (1-ci) * AZ{i-1}
 *    P_i(AZ|HW) = P(AZ|HW) * ci * HW{i-1}     = tAZ * ci * (1 - AZ{i-1})
 *
 *    P_i(HW|HW) = P(HW|HW) * (1-ci) * HW{i-1} = (1-tAZ) * (1-ci) * (1 - AZ{i-1})
 *    P_i(HW|AZ) = P(HW|AZ) * ci * AZ{i-1}     = tHW * ci * AZ{i-1}
 */

static void flush_viterbi(args_t *args)
{
    if ( !args->nsites ) return; 

    if ( !args->vi_training )
        hmm_run_viterbi(args->hmm, args->nsites, args->eprob, args->sites);
    else
    {
        double t2az_prev, t2hw_prev;
        do
        {
            t2az_prev = args->t2AZ;
            t2hw_prev = args->t2HW;
            hmm_run_viterbi(args->hmm, args->nsites, args->eprob, args->sites);
            update_tprobs(args->hmm);
            int i,j;
            for (i=0; i<2; i++)
            {
                for (j=0; j<2; j++) fprintf(stderr," %f", MAT(args->hmm->tprob,2,i,j));
            }
            fprintf(stderr,"\n");
        }
        while ( 1 ) ; //fabs(args->t2az-t2az_prev)>t2az_prev*1e-5 || fabs(args->t2hw-t2hw_prev)>t2hw_prev*1e-5 );
    }

    const char *chr = bcf_hdr_id2name(args->hdr,args->prev_rid);

    int i;
    for (i=0; i<args->nsites; i++)
    {
        printf("%s\t%d\t%d\t..\n", chr,args->sites[i]+1,args->hmm->vpath[i*2]==STATE_AZ ? 1 : 0);
    }
}

static int read_AF(args_t *args, bcf1_t *line, double *alt_freq)
{
    bcf_sr_regions_t *tgt = args->files->targets;
    if ( tgt->nals != line->n_allele ) return -1;    // number of alleles does not match

    int i;
    for (i=0; i<tgt->nals; i++)
        if ( strcmp(line->d.allele[i],tgt->als[i]) ) break; // we could be smarter, see vcmp
    if ( i<tgt->nals ) return -1;

    char *tmp, *str = tgt->line.s;
    i = 0;
    while ( *str && i<3 ) 
    {
        if ( *str=='\t' ) i++;
        str++;
    }
    *alt_freq = strtod(str, &tmp);
    if ( *tmp && !isspace(*tmp) )
    {
        if ( str[0]=='.' && (!str[1] || isspace(str[1])) ) return -1; // missing value
        error("Could not parse: [%s]\n", tgt->line.s);
    }
    if ( *alt_freq<0 || *alt_freq>1 ) error("Could not parse AF: [%s]\n", tgt->line.s);
    return 0;
}

int estimate_AF(args_t *args, bcf1_t *line, double *alt_freq)
{
    if ( !args->nitmp )
    {
        args->nitmp = bcf_get_genotypes(args->hdr, line, &args->itmp, &args->mitmp);
        if ( args->nitmp != 2*args->nsmpl ) return -1;     // not diploid?
        args->nitmp /= args->nsmpl;
    }

    int i, nalt = 0, nref = 0;
    for (i=0; i<args->nsmpl; i++)
    {
        int32_t *gt = &args->itmp[i*args->nitmp];

        if ( gt[0]==bcf_gt_missing || gt[1]==bcf_gt_missing ) continue;

        if ( bcf_gt_allele(gt[0]) ) nalt++;
        else nref++;

        if ( bcf_gt_allele(gt[1]) ) nalt++;
        else nref++;
    }
    if ( !nalt && !nref ) return -1;

    *alt_freq = (double)nalt / (nalt + nref);
    return 0;
}


int parse_line(args_t *args, bcf1_t *line, double *alt_freq, double *pdg)
{
    args->nitmp = 0;

    // Set allele frequency
    int ret;
    if ( args->af_tag )
    {
        // Use an INFO tag provided by the user
        ret = bcf_get_info_float(args->hdr, line, args->af_tag, &args->AFs, &args->mAFs);
        if ( ret==1 )
            *alt_freq = args->AFs[0];
        if ( ret==-2 )
            error("Type mismatch for INFO/%s tag at %s:%d\n", args->af_tag, bcf_seqname(args->hdr,line), line->pos+1);
    }
    else if ( args->af_fname ) 
    {
        // Read AF from a file
        ret = read_AF(args, line, alt_freq);
    }
    else
    {
        // Use GTs or AC/AN: GTs when AC/AN not present or when GTs explicitly requested by --estimate-AF
        ret = -1;
        if ( !args->estimate_AF )
        {
            int AC = -1, AN = 0;
            ret = bcf_get_info_int32(args->hdr, line, "AN", &args->itmp, &args->mitmp);
            if ( ret==1 )
            {
                AN = args->itmp[0];
                ret = bcf_get_info_int32(args->hdr, line, "AC", &args->itmp, &args->mitmp);
                if ( ret>0 )
                    AC = args->itmp[0];
            }
            if ( AN<=0 || AC<0 ) 
                ret = -1;
            else 
                *alt_freq = (double) AC/AN;
        }
        if ( ret==-1 )
            ret = estimate_AF(args, line, alt_freq);    // reads GTs into args->itmp
    }

    if ( ret<0 ) return ret;


    // Set P(D|G)
    if ( args->fake_PLs )
    {
        if ( !args->nitmp )
        {
            args->nitmp = bcf_get_genotypes(args->hdr, line, &args->itmp, &args->mitmp);
            if ( args->nitmp != 2*args->nsmpl ) return -1;     // not diploid?
            args->nitmp /= args->nsmpl;
        }

        int32_t *gt = &args->itmp[args->ismpl*args->nitmp];
        if ( gt[0]==bcf_gt_missing || gt[1]==bcf_gt_missing ) return -1;

        int a = bcf_gt_allele(gt[0]);
        int b = bcf_gt_allele(gt[1]);
        if ( a!=b )
        {
            pdg[0] = pdg[2] = args->unseen_PL;
            pdg[1] = 1 - 2*args->unseen_PL;
        }
        else if ( a==0 )
        {
            pdg[0] = 1 - 2*args->unseen_PL;
            pdg[1] = pdg[2] = args->unseen_PL;
        }
        else
        {
            pdg[0] = pdg[1] = args->unseen_PL;
            pdg[2] = 1 - 2*args->unseen_PL;
        }
    }
    else
    {
        args->nitmp = bcf_get_format_int32(args->hdr, line, "PL", &args->itmp, &args->mitmp);
        if ( args->nitmp != args->nsmpl*line->n_allele*(line->n_allele+1)/2. ) return -1;     // not diploid?
        args->nitmp /= args->nsmpl;

        int32_t *pl = &args->itmp[args->ismpl*args->nitmp];
        pdg[0] = args->pl2p[ pl[0] ];
        pdg[1] = args->pl2p[ pl[1] ];
        pdg[2] = args->pl2p[ pl[2] ];

        double sum = pdg[0] + pdg[1] + pdg[2];
        if ( !sum ) return -1;
        pdg[0] /= sum;
        pdg[1] /= sum;
        pdg[2] /= sum;
    }

    return 0;
}

static void vcfroh(args_t *args, bcf1_t *line)
{
    // Are we done?
    if ( !line )
    { 
        flush_viterbi(args);
        return; 
    }
    args->ntot++;

    // Skip unwanted lines
    if ( line->rid == args->skip_rid ) return;
    if ( line->n_allele==1 ) return;    // no ALT allele
    if ( line->n_allele!=2 ) return;    // only biallelic sites
    if ( args->snps_only && !bcf_is_snp(line) ) return;

    // Initialize genetic map
    int skip_rid = 0;
    if ( args->prev_rid<0 ) 
    {
        args->prev_rid = line->rid;
        args->prev_pos = line->pos;
        skip_rid = load_genmap(args, line);
    }

    // New chromosome?
    if ( args->prev_rid!=line->rid )
    {
        flush_viterbi(args);
        skip_rid = load_genmap(args, line);
        args->prev_rid = line->rid;
        args->prev_pos = line->pos;
    }

    if ( skip_rid )
    {
        fprintf(stderr,"Skipping the sequence, no genmap for %s\n", bcf_seqname(args->hdr,line));
        args->skip_rid = line->rid;
        return;
    }
    if ( args->prev_pos > line->pos ) error("The file is not sorted?!\n");

    args->prev_rid = line->rid;
    args->prev_pos = line->pos;


    // Ready for the new site
    int m = args->msites;
    hts_expand(uint32_t,args->nsites+1,args->msites,args->sites);
    if ( args->msites!=m )
        args->eprob = (double*) realloc(args->eprob,sizeof(double)*args->msites*2);

    // Set likelihoods and alternate allele frequencies
    double alt_freq, pdg[3];
    if ( parse_line(args, line, &alt_freq, pdg)<0 ) return; // something went wrong

    args->nused++;

    // Calculate emission probabilities P(D|AZ) and P(D|HW)
    double *eprob = &args->eprob[2*args->nsites];
    eprob[STATE_AZ] = pdg[0]*(1-alt_freq) + pdg[2]*alt_freq;
    eprob[STATE_HW] = pdg[0]*(1-alt_freq)*(1-alt_freq) + 2*pdg[1]*(1-alt_freq)*alt_freq + pdg[2]*alt_freq*alt_freq;

    args->sites[args->nsites] = line->pos;
    args->nsites++;
}

static void usage(args_t *args)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "About:   HMM model for detecting runs of autozygosity.\n");
    fprintf(stderr, "Usage:   bcftools roh [options] <in.vcf.gz>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "General Options:\n");
    fprintf(stderr, "        --AF-tag <TAG>                 use TAG for allele frequency\n");
    fprintf(stderr, "        --AF-file <file>               read allele frequencies from file (CHR\\tPOS\\tREF,ALT\\tAF)\n");
    fprintf(stderr, "    -e, --estimate-AF <file>           calculate AC,AN counts on the fly, using either all samples (\"-\") or samples listed in <file>\n");
    fprintf(stderr, "    -G, --GTs-only <float>             use GTs, ignore PLs, use <float> for PL of unseen genotypes. Safe value to use is 30 to account for GT errors.\n");
    fprintf(stderr, "    -I, --skip-indels                  skip indels as their genotypes are enriched for errors\n");
    fprintf(stderr, "    -m, --genetic-map <file>           genetic map in IMPUTE2 format, single file or mask, where string \"{CHROM}\" is replaced with chromosome name\n");
    fprintf(stderr, "    -r, --regions <region>             restrict to comma-separated list of regions\n");
    fprintf(stderr, "    -R, --regions-file <file>          restrict to regions listed in a file\n");
    fprintf(stderr, "    -s, --sample <sample>              sample to analyze\n");
    fprintf(stderr, "    -t, --targets <region>             similar to -r but streams rather than index-jumps\n");
    fprintf(stderr, "    -T, --targets-file <file>          similar to -R but streams rather than index-jumps\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "HMM Options:\n");
    fprintf(stderr, "    -a, --hw-to-az <float>             P(AZ|HW) transition probability from AZ (autozygous) to HW (Hardy-Weinberg) state [1e-8]\n");
    fprintf(stderr, "    -H, --az-to-hw <float>             P(HW|AZ) transition probability from HW to AZ state [1e-7]\n");
    fprintf(stderr, "    -V, --viterbi-training             perform Viterbi training to estimate transition probabilities\n");
    fprintf(stderr, "\n");
    exit(1);
}

int main_vcfroh(int argc, char *argv[])
{
    int c;
    args_t *args  = (args_t*) calloc(1,sizeof(args_t));
    args->argc    = argc; args->argv = argv;
    args->files   = bcf_sr_init();
    args->t2AZ    = 1e-8;
    args->t2HW    = 1e-7;
    int regions_is_file = 0, targets_is_file = 0;

    static struct option loptions[] = 
    {
        {"AF-tag",1,0,0},
        {"AF-file",1,0,1},
        {"estimate-AF",1,0,'e'},
        {"GTs-only",1,0,'G'},
        {"sample",1,0,'s'},
        {"hw-to-az",1,0,'a'},
        {"az-to-hw",1,0,'H'},
        {"viterbi-training",0,0,'V'},
        {"targets",1,0,'t'},
        {"targets-file",1,0,'T'},
        {"regions",1,0,'r'},
        {"regions-file",1,0,'R'},
        {"genetic-map",1,0,'m'},
        {"skip-indels",0,0,'I'},
        {0,0,0,0}
    };

    int naf_opts = 0;
    while ((c = getopt_long(argc, argv, "h?r:R:t:T:H:a:s:m:G:Ia:e:V",loptions,NULL)) >= 0) {
        switch (c) {
            case 0: args->af_tag = optarg; naf_opts++; break;
            case 1: args->af_fname = optarg; naf_opts++; break;
            case 'e': args->estimate_AF = optarg; naf_opts++; break;
            case 'I': args->snps_only = 1; break;
            case 'G': args->fake_PLs = 1; args->unseen_PL = pow(10,-atof(optarg)/10.); break;
            case 'm': args->genmap_fname = optarg; break;
            case 's': args->sample = optarg; break;
            case 'a': args->t2AZ = atof(optarg); break;
            case 'H': args->t2HW = atof(optarg); break;
            case 't': args->targets_list = optarg; break;
            case 'T': args->targets_list = optarg; targets_is_file = 1; break;
            case 'r': args->regions_list = optarg; break;
            case 'R': args->regions_list = optarg; regions_is_file = 1; break;
            case 'V': args->vi_training = 1; break;
            case 'h': 
            case '?': usage(args); break;
            default: error("Unknown argument: %s\n", optarg);
        }
    }

    if ( argc<optind+1 ) usage(args);
    if ( args->t2AZ<0 || args->t2AZ>1 ) error("Error: The parameter --hw-to-az is not in [0,1]\n", args->t2AZ);
    if ( args->t2HW<0 || args->t2HW>1 ) error("Error: The parameter --az-to-hw is not in [0,1]\n", args->t2HW);
    if ( naf_opts>1 ) error("Error: The options --AF-tag, --AF-file and -e are mutually exclusive\n");
    if ( args->af_fname && args->targets_list ) error("Error: The options --AF-file and -t are mutually exclusive\n");
    if ( !args->sample ) error("Missing the option -s, --sample\n");
    if ( args->regions_list )
    {
        if ( bcf_sr_set_regions(args->files, args->regions_list, regions_is_file)<0 )
            error("Failed to read the regions: %s\n", args->regions_list);
    }
    if ( args->targets_list )
    {
        if ( bcf_sr_set_targets(args->files, args->targets_list, targets_is_file, 0)<0 )
            error("Failed to read the targets: %s\n", args->targets_list);
    }
    if ( args->af_fname )
    {
        if ( bcf_sr_set_targets(args->files, args->af_fname, 1, 3)<0 )
            error("Failed to read the targets: %s\n", args->af_fname);
    }
    if ( !bcf_sr_add_reader(args->files, argv[optind]) ) error("Failed to open or the file not indexed: %s\n", argv[optind]);
    
    init_data(args);
    while ( bcf_sr_next_line(args->files) )
    {
        vcfroh(args, args->files->readers[0].buffer[0]);
    }
    vcfroh(args, NULL);
    fprintf(stderr,"Number of lines: total/processed: %d/%d\n", args->ntot,args->nused);
    destroy_data(args);
    free(args);
    return 0;
}


