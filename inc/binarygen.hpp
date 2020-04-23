// This file is part of PRSice-2, copyright (C) 2016-2019
// Shing Wan Choi, Paul F. O’Reilly
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef BinaryGEN_H
#define BinaryGEN_H

#include "bgen_lib.hpp"
#include "genotype.hpp"
#include "reporter.hpp"
#include <stdexcept>
#include <zlib.h>

/**
 * Potential problem:
 * Where multi-allelic variants exist in these data, they have been
 * split into a series of bi-allelic variants. This implies that
 * several variants may share the same genomic position but with
 * different alternative alleles.
 */
class BinaryGen : public Genotype
{
public:
    BinaryGen() {}
    BinaryGen(const GenoFile& geno, const Phenotype& pheno,
              const std::string& delim, Reporter* reporter);
    ~BinaryGen();

    //
    /*!
     * \brief check if the sample file is of the sample format specified by bgen
     *        or just a simple text file
     * \return
     */
    static bool check_is_sample_format(std::unique_ptr<std::istream>& input);

protected:
    typedef std::vector<std::vector<double>> Data;
    std::vector<genfile::bgen::Context> m_context_map;
    std::vector<genfile::byte_t> m_buffer1, m_buffer2;
    bool m_target_plink = false;
    bool m_ref_plink = false;
    bool m_has_external_sample = false;

    /*!
     * \brief Generate the sample vector
     * \return Vector containing the sample information
     */
    std::vector<Sample_ID> gen_sample_vector();
    void handle_pheno_header(std::unique_ptr<std::istream>& sample);
    void
    gen_snp_vector(const std::vector<IITree<size_t, size_t>>& exclusion_regions,
                   const std::string& out_prefix, Genotype* target = nullptr);
    bool calc_freq_gen_inter(const QCFiltering& filter_info,
                             const std::string& prefix,
                             Genotype* genotype = nullptr);

    genfile::bgen::Context get_context(const size_t& idx);
    size_t get_sex_col(const std::string& header,
                       const std::string& format_line);
    /*!
     * \brief Check if the sample information and ordering of the bgen file
     *        matched the sample / phenotype file
     * \param bgen_name is the name of the bgen file
     * \param context is the context object
     * \return true if the sample is consistent
     */
    void check_sample_consistent(const genfile::bgen::Context& context,
                                 std::istream& stream);

    size_t transverse_bgen_for_snp(
        const std::vector<IITree<size_t, size_t>>& exclusion_regions,
        const std::string mismatch_snp_record_name, const size_t file_idx,
        std::unique_ptr<std::istream> bgen_file,
        std::unordered_set<std::string>& duplicated_snps,
        std::unordered_set<std::string>& processed_snps,
        std::vector<bool>& retain_snp, bool& chr_error, bool& sex_error,
        Genotype* genotype);
    /*!
     * \brief This function will read in the bgen probability data and transform
     * it into PLINK bianry
     *
     * \param genotype is the vector containing the plink binary
     * \param byte_pos is the streampos of the bgen file, for quick seek
     * \param file_name is the file name of the bgen file
     */
    inline void read_genotype(uintptr_t* genotype,
                              const std::streampos byte_pos,
                              const size_t& file_idx)
    {
        const uintptr_t unfiltered_sample_ct4 =
            (m_unfiltered_sample_ct + 3) / 4;
        if (m_ref_plink)
        {
            // when m_ref_plink is set, it suggest we are using the
            // intermediate, which is a binary plink format. Therefore we can
            // directly read in the binary data to genotype, this should already
            // be well formated when we write it into the file
            // intermediate file, so don't need .bgen
            std::string file_name = m_genotype_file_names[file_idx];
            m_genotype_file.read(file_name, byte_pos, unfiltered_sample_ct4,
                                 reinterpret_cast<char*>(genotype));
        }
        // if not, we will try to parse the binary GEN format into a plink
        // format (using the default intermediate flag = false)
        else if (load_and_collapse_incl(byte_pos, file_idx, genotype))
        {
            throw std::runtime_error("Error: Cannot read the bgen file!");
        }
    }

    /*!
     * \brief We modify the load_and_collapse_incl function from PLINK to read
     * in and reformat the bgen data into the binary PLINK data
     *
     * \param byte_pos is the location of the file
     * \param file_name is the file name
     * \param unfiltered_sample_ct is the number of unfiltered sample
     * \param sample_ct is the number of sample we want
     * \param sample_include is the bianry vector containing the boolean
     * indicate if we want the target sample
     *
     * \param final_mask is a mask to remove trailing bytes
     * \param do_reverse is implemented in PLINK but we will not use
     * \param rawbuf is the raw vector to store the information
     * \param mainbuf is the result vector containing the genotype information
     * \param intermediate is boolean indicate if we want to generate an
     * intermediate file
     * \return 0 if sucessful
     */
    uint32_t load_and_collapse_incl(const std::streampos byte_pos,
                                    const size_t& file_idx,
                                    uintptr_t* __restrict mainbuf)
    {
        // check sample size != 0
        assert(m_unfiltered_sample_ct);
        // we first check if we will read in a new file
        auto&& context = m_context_map[file_idx];
        // we initailize the PLINK generator with the m_sample_include
        // vector, the mainbuf (result storage) and also the hard threshold.
        // We don't need to bother about founder or founder info here as all
        // samples in bgen are considered as founder (at least this is the
        // case at the moment)
        // TODO, might want to make this struct a member so that we don't
        // need to re-initialize it? Though it only contain pointers and
        // doesn't have any big structures
        PLINK_generator setter(&m_calculate_prs, mainbuf, m_hard_threshold,
                               m_dose_threshold);
        // we can now use the bgen library to parse the BGEN input and
        // transform it into PLINK format (NOTE: The
        // read_and_parse_genotype_data_block function has been modified
        // such that it will always call .sample_completed() when finish
        // reading each sample. This allow for a more elegant implementation
        // on our side
        std::string file_name = m_genotype_file_names[file_idx] + ".bgen";
        // WARNING: Problem here
        genfile::bgen::read_and_parse_genotype_data_block<PLINK_generator>(
            m_genotype_file, file_name, context, setter, &m_buffer1, &m_buffer2,
            byte_pos);
        // output from load_raw should have already copied all samples
        // to the front without the need of subseting
        // mainbuf should contains the information
        // update the m_prev_loc counter. However, for bgen, it is most likely
        // that we will need to seek for every SNP as there are padded data
        // between each SNP's genotype
        return 0;
    }

    void read_score(std::vector<PRS>& prs_list,
                    const std::vector<size_t>::const_iterator& start_idx,
                    const std::vector<size_t>::const_iterator& end_idx,
                    bool reset_zero, bool ultra = false);
    void hard_code_score(std::vector<PRS>& prs_list,
                         const std::vector<size_t>::const_iterator& start_idx,
                         const std::vector<size_t>::const_iterator& end_idx,
                         bool reset_zero, bool ultra = false);
    void dosage_score(std::vector<PRS>& prs_list,
                      const std::vector<size_t>::const_iterator& start_idx,
                      const std::vector<size_t>::const_iterator& end_idx,
                      bool reset_zero);

    /*
     * Different structures use for reading in the bgen info
     */
    // TODO: Use ref MAf for dosage score too
    struct PRS_Interpreter
    {
        ~PRS_Interpreter() {}
        /*!
         * \brief PRS_Interpreter is the structure used by BGEN library to parse
         * the probability data
         *
         * \param sample_prs is the vector where we store the results
         * \param sample_inclusion is the vector telling us if the sample is
         * required
         *
         * \param missing contain the method of missingness handling
         */
        PRS_Interpreter(std::vector<PRS>* sample_prs,
                        std::vector<uintptr_t>* sample_inclusion,
                        MISSING_SCORE missing)
            : m_sample_prs(sample_prs), m_sample_inclusion(sample_inclusion)
        {
            m_miss_count = m_ploidy * (missing != MISSING_SCORE::SET_ZERO);
            // to account for the missingness, we need to calculate the mean of
            // the PRS before we can assign the missing value to the sample. As
            // a result of that, we need a vector to store the index of the
            // missing sample. The maximum possible number of missing sample is
            // the number of sample, thus we can reserve the required size
            m_setzero = (missing == MISSING_SCORE::SET_ZERO);
            m_centre = (missing == MISSING_SCORE::CENTER);
        }
        /*!
         * \brief As we will reuse this struct in our analysis, we need to
         * constantly feed in different statistics and to inform this struct
         * whether we want to reset the PRS
         *
         * \param stat is the effect size of the SNP
         * \param homcom_weight is the weight of the homozygous common variant
         * \param het_weight is the weight of heterozygous
         * \param homrar_weight is the weight of homozygous rare variant
         * \param flipped represent whether we want to flip this SNP
         * \param not_first inform us if we want to construct a new score or not
         */
        void set_stat(const double& stat, const double& homcom_weight,
                      const double& het_weight, const double& homrar_weight,
                      const bool flipped, const bool not_first)
        {
            // don't use external expected as that doesn't take into account of
            // the weighting
            m_missing.clear();
            m_stat = stat;
            m_homcom_weight = homcom_weight;
            m_het_weight = het_weight;
            m_homrar_weight = homrar_weight;
            m_not_first = not_first;
            m_cal_expected = 0;
            rs.clear();
            // to match the encoding in PLINK format, we "unflip" SNPs here
            // otherwise our polygenic score will be going to an opposite
            // direction
            if (!flipped)
            {
                // immediately flip the weight at the beginning
                std::swap(m_homcom_weight, m_homrar_weight);
            }
            m_adj_score = 0;
            m_miss_score = 0;
            m_miss_count = 0;
            if (!m_setzero)
            {
                // this is the only one that depends on ploidy
                m_miss_count = 2;
                // again, mean_impute is stable, branch prediction should be ok
                // 0 if we don't have the expected score
            }
        }
        /*!
         * \brief This function is called by BGEN whenever a new SNP is
         * encountered
         */
        void initialise(std::size_t, std::size_t)
        {
            // we will reset the IDs
            // m_prs_sample_i represent the sample index of the result PRS
            // vector which does not contain samples that are removed
            m_prs_sample_i = 0;
        }
        /*!
         * \brief Another function mandated by bgen library that we don't use
         */
        void set_min_max_ploidy(uint32_t, uint32_t, uint32_t, uint32_t) {}

        /*!
         * \brief Function to set the current sample ID and return whether we
         * want to decompose the probability of this sample
         *
         * \param i is the sample index w.r.t the bgen file
         * \return true if we want to process this sample
         */
        bool set_sample(std::size_t i)
        {
            // a flag to indicate if this is a missing sample
            m_is_missing = false;
            // a flag to indicate this is the first genotype of the sample
            // we set this to account for the off chance where the set_value
            // isn't called in the expected order (though in that case, our
            // m_entry_i might also be wrong)
            m_start_geno = false;
            // we store the weighted sum of the genotypes to obtain the final
            // dosage
            m_sum = 0.0;
            // and we need to keep track of the sum of probability to check if
            // we have encountered missing genotype for V11 which is represented
            // by a sum of probability = 0
            m_sum_prob = 0.0;
            // if the byte is set, then we want this sample
            return IS_SET(m_sample_inclusion->data(), i);
        }
        void set_number_of_entries(std::size_t ploidy, std::size_t,
                                   genfile::OrderType phased,
                                   genfile::ValueType)
        {
            m_ploidy = ploidy;
            m_phased = phased;
        }

        // Called once for each genotype (or haplotype) probability per sample.
        /*!
         * \brief This function is called for each genotype for each sample.
         * \param geno  is the genotype dosage w.r.t the last allele
         * \param value is the probability of the allele
         */
        void set_value(uint32_t geno, double value)
        {
            // as this function is called multiple times per sample, to avoid
            // adding up three times the SNP number, and to account for the
            // missingness (bgen v1.1), we will first store the sample PRS in
            // the double sum and only add it to the sample if it is not missing
            if (m_phased == genfile::OrderType::ePerPhasedHaplotypePerAllele
                && geno > 1)
            {
                switch (geno)
                {
                case 3: geno = 2; break;
                default: geno = 0;
                }
            }
            switch (geno)
            {
            default: m_sum += m_homcom_weight * value; break;
            case 1: m_sum += m_het_weight * value; break;
            case 2: m_sum += m_homrar_weight * value; break;
            }
            m_sum_prob += value;
        }
        /*!
         * \brief For bgen v1.2 or later, when there is a missing sample, a
         * missing signature will be called which leads to this set_value
         * function instead
         */
        void set_value(uint32_t, genfile::MissingValue) { m_is_missing = true; }
        /*!
         * \brief This function is called whenever all probability for a sample
         * are read. We can then perform assignment to the sample
         */
        void sample_completed()
        {
            auto&& sample_prs = (*m_sample_prs)[m_prs_sample_i];

            if (misc::logically_equal(m_sum_prob, 0.0) || m_is_missing)
            {
                m_missing.push_back(m_prs_sample_i);
                sample_prs.num_snp =
                    sample_prs.num_snp * m_not_first + m_miss_count;
            }
            // this is not a missing sample and we can either add the prs or
            // assign the PRS
            else
            {
                // this is not the first SNP in the region, we will add
                sample_prs.num_snp =
                    sample_prs.num_snp * m_not_first + m_ploidy;
                sample_prs.prs =
                    sample_prs.prs * m_not_first + m_sum * m_stat - m_adj_score;
                rs.push(m_sum);
            }
            // go to next sample that we need (not the bgen index)
            ++m_prs_sample_i;
        }
        /*!
         * \brief once we finish reading the whole SNP, we can then start
         * processing samples with missing data. This requires us to calculate
         * the expected value and use that as the PRS of the samples
         */
        void finalise()
        {
            if (m_centre) { m_adj_score = m_stat * rs.mean(); }
            if (!m_setzero)
            {
                m_miss_count = 2;
                m_miss_score = m_stat * rs.mean();
            }

            // only need to do this if we don't have the expected information
            size_t cur_idx = 0;
            for (size_t i = 0; i < m_sample_prs->size(); ++i)
            {
                if (cur_idx < m_missing.size() && i == m_missing[cur_idx])
                {
                    (*m_sample_prs)[i].prs =
                        (*m_sample_prs)[i].prs * m_not_first + m_miss_score;
                    ++cur_idx;
                }
                else if (m_centre)
                {
                    // if it is not missing and we want the centre the score
                    // we will need to minus the adjusted score which was 0
                    // before this run
                    (*m_sample_prs)[i].prs -= m_adj_score;
                }
            }
        }

    private:
        std::vector<PRS>* m_sample_prs;
        std::vector<uintptr_t>* m_sample_inclusion;
        std::vector<size_t> m_missing;
        misc::RunningStat rs;
        genfile::OrderType m_phased = genfile::OrderType::ePerUnorderedGenotype;
        double m_stat = 0.0;
        double m_sum = 0.0;
        double m_sum_prob = 0.0;
        double m_homcom_weight = 0;
        double m_het_weight = 0.1;
        double m_homrar_weight = 1;
        double m_miss_score = 0.0;
        double m_adj_score = 0.0;
        double m_cal_expected = 0.0;
        uint32_t m_prs_sample_i = 0;
        int m_miss_count = 0;
        int m_ploidy = 2;
        bool m_not_first = false;
        bool m_is_missing = false;
        bool m_start_geno = false;
        bool m_setzero = false;
        bool m_centre = false;
    };

    struct PLINK_generator
    {
        PLINK_generator(std::vector<uintptr_t>* sample, uintptr_t* genotype,
                        double hard_threshold, double dose_threshold)
            : m_sample(sample)
            , m_genotype(genotype)
            , m_hard_threshold(hard_threshold)
            , m_dose_threshold(dose_threshold)
        {
            m_prob.resize(3);
        }
        void initialise(std::size_t, std::size_t)
        {
            // initialize is called when we meet a new SNP
            // we therefore want to reset the index and shift, use for
            // indicate where we push the byte onto genotype
            m_index = 0;
            m_shift = 0;
            m_homcom_ct = 0;
            m_homrar_ct = 0;
            m_het_ct = 0;
            m_missing_ct = 0;
            m_impute2 = 0;
            // we also clean the running stat, so that we can go
            // through another round of calculation of mean and sd
            statistic.clear();
        }
        void set_min_max_ploidy(uint32_t, uint32_t, uint32_t, uint32_t) {}
        /*!
         * \brief set_sample is the function to be called when BGEN start to
         * process a new sample
         *
         * \param i is the ID of the sample (index on vector)
         * \return True if we want to process this sample, false if not
         */
        bool set_sample(std::size_t i)
        {
            // we set the sample index to i
            m_sample_i = i;
            // we also reset the hard_prob to 0
            m_hard_prob = 0.0;
            // and expected value to 0
            m_missing = false;
            // then we determine if we want to include sample using by
            // consulting the flag on m_sample
            return IS_SET(m_sample->data(), m_sample_i);
        }
        void set_number_of_entries(std::size_t, std::size_t,
                                   genfile::OrderType phased,
                                   genfile::ValueType)
        {
            m_phased =
                (phased == genfile::OrderType::ePerPhasedHaplotypePerAllele);
            m_prob.resize(3 + m_phased, 0.0);
        }

        // Called once for each genotype (or haplotype) probability per
        // sample.
        /*!
         * \brief set_value is called for each genotype or haplotype
         * probability per sample \param value where value is the
         * probability of the genotype
         */
        void set_value(uint32_t idx, double value)
        {
            // if the current probability is the highest and the value is
            // higher than the required threshold, we will assign it
            //            if (value > m_hard_prob && value >= m_hard_threshold)
            //            {
            //                /*
            //                 * Representation of each geno to their binary
            //                 code:
            //                 *   geno    desired binary  decimal
            //                 representation
            //                 *   0           00              0
            //                 *   1           01              1
            //                 *   2           11              3
            //                 *   the binary code 10 is reserved for missing
            //                 sample
            //                 */
            //                // plink do 3 2 0 1
            //                m_geno = (geno == 2) ? 3 : geno;
            //                m_hard_prob = value;
            //            }
            // TODO: To account for situation where there are more than 3
            // genotype (which shouldn't happen to be honest)

            // when data is phased, BGEN Lib will give us 4 probabbilities:
            // prob of allele 1 in hap 1, prob of allele 2 in hap 1 and then
            // prob of allele 1 in hap 2 and then prob of allele 2 in hap 2,
            // which is slightly different from what was stated in the bgen
            // format as the bgen lib api automatically do the 1-prob allele 1
            // to give us the prob of allele 2
            m_prob[idx] = value;
        }

        void set_value(uint32_t, genfile::MissingValue) { m_missing = true; }
        void finalise() {}
        /*!
         * \brief sample_completed is called when each sample is completed.
         * This will set the binary vector accordingly
         */
        void sample_completed()
        {
            // if m_shift is zero, it is when we meet the index for the
            // first time, therefore we want to initialize it
            if (m_shift == 0) m_genotype[m_index] = 0;
            // check missing not required for phased data, as phased data only
            // supported in 1.2+, which represent missing value differently
            if (!m_phased && !m_missing)
            {
                m_missing = misc::logically_equal(
                    m_prob[0] + m_prob[1] + m_prob[2], 0.0);
                m_geno_prob = m_prob;
            }
            else
            {
                // convert haplotype prob into genotype prob
                m_geno_prob.resize(3, 0.0);
                m_geno_prob[0] = m_prob[0] * m_prob[2];
                m_geno_prob[1] = m_prob[0] * m_prob[3] + m_prob[1] * m_prob[2];
                m_geno_prob[2] = m_prob[1] * m_prob[3];
            }
            double impute2_tmp = 0.0;
            m_exp_value = 0;
            for (size_t i = 0; i < 3; ++i)
            {
                m_exp_value += m_geno_prob[i] * i;
                impute2_tmp += m_geno_prob[i] * i * i;
            }
            m_impute2 += impute2_tmp - m_exp_value;

            const double prob1 = m_geno_prob[0] * 2 + m_geno_prob[1];
            const double prob2 = m_geno_prob[2] * 2 + m_geno_prob[1];
            uintptr_t obs_genotype = 1;
            const double hard_score = (std::fabs(prob1 - std::round(prob1))
                                       + std::fabs(prob2 - std::round(prob2)))
                                      * 0.5;
            if (!m_missing && (hard_score <= m_hard_threshold))
            {
                m_hard_prob = 0;
                for (size_t geno = 0; geno < 3; ++geno)
                {
                    if (m_prob[geno] > m_hard_prob
                        && m_prob[geno] >= m_dose_threshold)
                    {
                        // +1 because geno ==1 represents missing
                        obs_genotype = (geno == 0) ? geno : geno + 1;
                        m_hard_prob = m_prob[geno];
                    }
                }
            }
            // we now add the genotype to the vector
            m_genotype[m_index] |= obs_genotype << m_shift;
            switch (obs_genotype)
            {
            case 3: ++m_homrar_ct; break;
            case 2: ++m_het_ct; break;
            case 1: ++m_missing_ct; break;
            case 0: ++m_homcom_ct; break;
            }
            // as the genotype is represented by two bit, we will +=2
            m_shift += 2;
            // if we reach the boundary, we will now add the index and reset
            // the shift
            if (m_shift == BITCT)
            {
                ++m_index;
                m_shift = 0;
            }
            // we can now push in the expected value for this sample. This
            // can then use for the calculation of the info score
            // only calculate for included samples
            if (IS_SET(m_sample->data(), m_sample_i))
                statistic.push(m_exp_value);
        }
        /*!
         * \brief info_score is the function use to calculate the info score
         * \return return the  MaCH INFO score
         */
        double info_score() const
        {
            double p = statistic.mean() / 2.0;
            double p_all = 2.0 * p * (1.0 - p);
            return (statistic.var() / p_all);
        }
        double impute_info_score() const
        {
            double p = statistic.mean() / 2.0;
            double p_all = 2.0 * p * (1.0 - p) * statistic.get_n();
            return 1 - (m_impute2 / p_all);
        }
        double expected() const { return statistic.mean(); }
        void get_count(size_t& homcom_ct, size_t& het_ct, size_t& homrar_ct,
                       size_t& missing_ct) const
        {
            homcom_ct = m_homcom_ct;
            het_ct = m_het_ct;
            homrar_ct = m_homrar_ct;
            missing_ct = m_missing_ct;
        }
        void get_count(uint32_t& homcom_ct, uint32_t& het_ct,
                       uint32_t& homrar_ct, uint32_t& missing_ct) const
        {
            homcom_ct = m_homcom_ct;
            het_ct = m_het_ct;
            homrar_ct = m_homrar_ct;
            missing_ct = m_missing_ct;
        }
        virtual ~PLINK_generator() {}

    private:
        // the sample inclusion vector, if bit is set, sample is required
        std::vector<uintptr_t>* m_sample;
        std::vector<double> m_prob;
        std::vector<double> m_geno_prob;
        // is the genotype vector
        uintptr_t* m_genotype;
        misc::RunningStat statistic;
        double m_hard_threshold = 0.0;
        double m_dose_threshold = 0.0;
        double m_hard_prob = 0;
        double m_exp_value = 0.0;
        double m_impute2 = 0.0;
        uint32_t m_shift = 0;
        uint32_t m_index = 0;
        size_t m_sample_i = 0;
        uint32_t m_homcom_ct = 0;
        uint32_t m_homrar_ct = 0;
        uint32_t m_het_ct = 0;
        uint32_t m_missing_ct = 0;
        bool m_phased = false;
        bool m_missing = false;
    };
};

#endif
