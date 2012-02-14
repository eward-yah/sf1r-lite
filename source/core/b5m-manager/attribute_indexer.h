#ifndef SF1R_B5MMANAGER_ATTRIBUTEINDEXER_H_
#define SF1R_B5MMANAGER_ATTRIBUTEINDEXER_H_
#include "ngram_synonym.h"
#include "b5m_types.h"
#include <boost/regex.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#include <boost/algorithm/string.hpp>
#include <ir/id_manager/IDManager.h>
#include <am/sequence_file/ssfr.h>
#include <am/leveldb/Table.h>
#include <idmlib/util/idm_analyzer.h>

namespace sf1r {

    class AttributeIndexer {


    public:

        typedef izenelib::ir::idmanager::_IDManager<AttribRep, AttribId,
                           izenelib::util::NullLock,
                           izenelib::ir::idmanager::EmptyWildcardQueryHandler<AttribRep, AttribId>,
                           izenelib::ir::idmanager::UniqueIDGenerator<AttribRep, AttribId>,
                           izenelib::ir::idmanager::HDBIDStorage<AttribRep, AttribId> >  AttribIDManager;
        typedef izenelib::ir::idmanager::_IDManager<AttribRep, AttribNameId,
                           izenelib::util::NullLock,
                           izenelib::ir::idmanager::EmptyWildcardQueryHandler<AttribRep, AttribNameId>,
                           izenelib::ir::idmanager::UniqueIDGenerator<AttribRep, AttribNameId>,
                           izenelib::ir::idmanager::HDBIDStorage<AttribRep, AttribNameId> >  AttribNameIDManager;
        //typedef izenelib::ir::idmanager::_IDManager<AttribRep, AttribId> AttribIDManager;
        //typedef izenelib::am::leveldb::Table<AttribValueId, std::vector<AttribId> > AttribIndex;
        typedef izenelib::am::leveldb::Table<izenelib::util::UString, std::vector<AttribId> > AttribIndex;
        typedef izenelib::am::leveldb::Table<AttribId, AttribNameId > AttribNameIndex;
        AttributeIndexer();
        ~AttributeIndexer();
        bool LoadSynonym(const std::string& file);
        bool Index(const std::string& scd_file, const std::string& knowledge_dir);
        bool Open(const std::string& knowledge_dir);
        void GetAttribIdList(const izenelib::util::UString& value, std::vector<AttribId>& id_list);
        void GetAttribIdList(const izenelib::util::UString& category, const izenelib::util::UString& value, std::vector<AttribId>& id_list);
        bool GetAttribRep(const AttribId& aid, AttribRep& rep);
        bool GetAttribNameId(const AttribId& aid, AttribNameId& name_id);
        void GenClassiferInstance();
        void ProductMatchingSVM(const std::string& scd_file, const std::string& match_file);
        void ProductMatchingLR(const std::string& scd_file);

        bool TrainSVM();

        void SetCmaPath(const std::string& path)
        { cma_path_ = path; }

        void SetCategoryRegex(const std::string& str)
        {
            match_param_.SetCategoryRegex(str);
        }

    private:
        void ClearKnowledge_(const std::string& knowledge_dir);
        inline AttribRep GetAttribRep(const izenelib::util::UString& category, const izenelib::util::UString& attrib_name, const izenelib::util::UString& attrib_value)
        {
            AttribRep result;
            result.append(category);
            result.append(izenelib::util::UString("|", izenelib::util::UString::UTF_8));
            result.append(attrib_name);
            result.append(izenelib::util::UString("|", izenelib::util::UString::UTF_8));
            result.append(attrib_value);
            return result;
        }

        inline void SplitAttribRep(const AttribRep& rep, izenelib::util::UString& category, izenelib::util::UString& attrib_name, izenelib::util::UString& attrib_value)
        {
            std::string srep;
            rep.convertString(srep, izenelib::util::UString::UTF_8);
            std::vector<std::string> words;
            boost::algorithm::split( words, srep, boost::algorithm::is_any_of("|") );
            category = izenelib::util::UString( words[0], izenelib::util::UString::UTF_8);
            attrib_name = izenelib::util::UString( words[1], izenelib::util::UString::UTF_8);
            attrib_value = izenelib::util::UString( words[2], izenelib::util::UString::UTF_8);
        }

        inline AttribRep GetAttribRep(const izenelib::util::UString& category, const izenelib::util::UString& attrib_name)
        {
            AttribRep result;
            result.append(category);
            result.append(izenelib::util::UString("|", izenelib::util::UString::UTF_8));
            result.append(attrib_name);
            return result;
        }

        inline void SplitAttribRep(const AttribRep& rep, izenelib::util::UString& category, izenelib::util::UString& attrib_name)
        {
            std::string srep;
            rep.convertString(srep, izenelib::util::UString::UTF_8);
            std::vector<std::string> words;
            boost::algorithm::split( words, srep, boost::algorithm::is_any_of("|") );
            category = izenelib::util::UString( words[0], izenelib::util::UString::UTF_8);
            attrib_name = izenelib::util::UString( words[1], izenelib::util::UString::UTF_8);
        }
        AttribValueId GetAttribValueId_(const izenelib::util::UString& av)
        {
            return izenelib::util::HashFunction<izenelib::util::UString>::generateHash64(av);
        }

        void BuildProductDocuments_(const std::string& scd_file);

        void Analyze_(const izenelib::util::UString& text, std::vector<izenelib::util::UString>& result);
        void AnalyzeChar_(const izenelib::util::UString& text, std::vector<izenelib::util::UString>& result);

        //av means 'attribute value'
        void NormalizeAV_(const izenelib::util::UString& av, izenelib::util::UString& nav);

        bool IsBoolAttribValue_(const izenelib::util::UString& av, bool& b);
        void GetAttribNameMap_(const std::vector<AttribId>& aid_list, boost::unordered_map<AttribNameId, std::vector<AttribId> >& map);
        void GetFeatureVector_(const std::vector<AttribId>& o, const std::vector<AttribId>& p, std::vector<std::pair<AttribNameId, double> >& feature_vec, const FeatureCondition& fc = FeatureCondition());
        void GenNegativeIdMap_(std::map<std::size_t, std::vector<std::size_t> >& map);
        void BuildCategoryMap_(boost::unordered_map<std::string, std::vector<std::size_t> >& map);
        void WriteIdInfo_();

    private:
        std::string cma_path_;
        boost::unordered_map<std::string, std::string> synonym_map_;
        NgramSynonym ngram_synonym_;
        bool is_open_;
        std::ofstream logger_;
        MatchParameter match_param_;
        std::string knowledge_dir_;
        AttribIndex* index_;
        AttribIDManager* id_manager_;
        AttribNameIndex* name_index_;
        AttribNameIDManager* name_id_manager_;
        idmlib::util::IDMAnalyzer* analyzer_;
        idmlib::util::IDMAnalyzer* char_analyzer_;
        std::vector<ProductDocument> product_list_;
        boost::unordered_set<AttribNameId> filter_anid_;
        boost::unordered_set<std::string> filter_attrib_name_;
    };
}

#endif
