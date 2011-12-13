#include "product_manager.h"
#include "product_data_source.h"
#include "operation_processor.h"
#include "uuid_generator.h"
#include "product_backup.h"
#include "product_clustering.h"
#include "product_price_trend.h"

#include <common/ScdWriter.h>
#include <common/Utilities.h>
#include <boost/unordered_set.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/dynamic_bitset.hpp>
using namespace sf1r;
using izenelib::util::UString;

// #define PM_PROFILER

ProductManager::ProductManager(
        const std::string& work_dir,
        ProductDataSource* data_source,
        OperationProcessor* op_processor,
        ProductPriceTrend* price_trend,
        const PMConfig& config)
    : work_dir_(work_dir)
    , data_source_(data_source)
    , op_processor_(op_processor)
    , price_trend_(price_trend)
    , clustering_(NULL)
    , backup_(NULL)
    , config_(config)
    , has_price_trend_(false)
    , inhook_(false)
{
    if (price_trend_)
    {
        if (price_trend_->Init())
            has_price_trend_ = true;
        else
            std::cerr << "Error: Price trend has not been properly initialized" << std::endl;
    }

    if (!config_.backup_path.empty())
    {
        backup_ = new ProductBackup(config_.backup_path);
    }
}

ProductManager::~ProductManager()
{
}

bool ProductManager::Recover()
{
    if (backup_==NULL)
    {
        error_ = "Backup not initialzed.";
        return false;
    }
    if (!backup_->Recover(this))
    {
//         error_ = "Backup recover failed.";
        return false;
    }
    if (!GenOperations_())
    {
        return false;
    }
    return true;
}

ProductClustering* ProductManager::GetClustering_()
{
    if(clustering_==NULL)
    {
        if(config_.enable_clustering_algo)
        {
            clustering_ = new ProductClustering(work_dir_+"/clustering", config_);
            if(!clustering_->Open())
            {
                std::cout<<"ProductClustering open failed"<<std::endl;
                delete clustering_;
                clustering_ = NULL;
            }
        }
    }
    return clustering_;
}

bool ProductManager::HookInsert(PMDocumentType& doc, izenelib::ir::indexmanager::IndexerDocument& index_document, time_t timestamp)
{
    inhook_ = true;
    boost::mutex::scoped_lock lock(human_mutex_);

    if (has_price_trend_)
    {
        ProductPrice price;
        if (GetPrice_(doc, price))
        {
            UString docid;
            if (GetDOCID_(doc, docid))
            {
                std::string docid_str;
                docid.convertString(docid_str, UString::UTF_8);
                if (timestamp == -1) GetTimestamp_(doc, timestamp);
                std::map<std::string, std::string> group_prop_map;
                GetGroupProperties_(doc, group_prop_map);
                price_trend_->Update(docid_str, price, timestamp, group_prop_map);
//              price_trend_->Insert(docid_str, price, timestamp);
            }
        }
    }
    ProductClustering* clustering = GetClustering_();
    if( clustering == NULL )
    {
        UString uuid;
        generateUUID(uuid, doc);
        if (!data_source_->SetUuid(index_document, uuid)) return false;
        PMDocumentType new_doc(doc);
        doc.property(config_.uuid_property_name) = uuid;
        new_doc.property(config_.docid_property_name) = uuid;
        SetItemCount_(new_doc, 1);
        op_processor_->Append(1, new_doc);
    }
    else
    {
        UString uuid;
        doc.property(config_.uuid_property_name) = uuid;
        if (!data_source_->SetUuid(index_document, uuid)) return false; // set a empty uuid for rtype update later
        clustering->Insert(doc);
    }

    return true;
}

bool ProductManager::HookUpdate(PMDocumentType& to, izenelib::ir::indexmanager::IndexerDocument& index_document, time_t timestamp, bool r_type)
{
    inhook_ = true;
    boost::mutex::scoped_lock lock(human_mutex_);

    uint32_t fromid = index_document.getId(); //oldid
    PMDocumentType from;
    if (!data_source_->GetDocument(fromid, from)) return false;
    UString from_uuid;
    if (!GetUuid_(from, from_uuid)) return false;
    ProductPrice from_price;
    ProductPrice to_price;
    GetPrice_(fromid, from_price);
    GetPrice_(to, to_price);

    if (has_price_trend_ && to_price.Valid() && from_price != to_price)
    {
        UString docid;
        if (GetDOCID_(to, docid))
        {
            std::string docid_str;
            docid.convertString(docid_str, UString::UTF_8);
            if (timestamp == -1) GetTimestamp_(to, timestamp);
            std::map<std::string, std::string> group_prop_map;
            GetGroupProperties_(to, group_prop_map);
            price_trend_->Update(docid_str, to_price, timestamp, group_prop_map);
        }
    }

    std::vector<uint32_t> docid_list;
    data_source_->GetDocIdList(from_uuid, docid_list, fromid); // except from.docid
    if (docid_list.empty()) // the from doc is unique, so delete it and insert 'to'
    {
        if (!data_source_->SetUuid(index_document, from_uuid)) return false;
        PMDocumentType new_doc(to);
        to.property(config_.uuid_property_name) = from_uuid;
        new_doc.property(config_.docid_property_name) = from_uuid;
        SetItemCount_(new_doc, 1);
        op_processor_->Append(2, new_doc);// if r_type, only numeric properties in 'to'
    }
    else
    {
        //need not to update(insert) to.uuid,
        if (!data_source_->SetUuid(index_document, from_uuid)) return false;
        to.property(config_.uuid_property_name) = from_uuid;
        //update price only
        if (from_price != to_price)
        {
            PMDocumentType diff_properties;
            ProductPrice price(to_price);
            GetPrice_(docid_list, price);
            diff_properties.property(config_.price_property_name) = price.ToUString();
            diff_properties.property(config_.docid_property_name) = from_uuid;
            //auto r_type? itemcount no need?
//                 diff_properties.property(config_.itemcount_property_name) = UString(boost::lexical_cast<std::string>(docid_list.size()+1), UString::UTF_8);

            op_processor_->Append(2, diff_properties);
        }
    }
    return true;
}

bool ProductManager::HookDelete(uint32_t docid, time_t timestamp)
{
    inhook_ = true;
    boost::mutex::scoped_lock lock(human_mutex_);
    PMDocumentType from;
    if (!data_source_->GetDocument(docid, from)) return false;
    UString from_uuid;
    if (!GetUuid_(from, from_uuid)) return false;
    std::vector<uint32_t> docid_list;
    data_source_->GetDocIdList(from_uuid, docid_list, docid); // except from.docid
    if (docid_list.empty()) // the from doc is unique, so delete it in A
    {
        PMDocumentType del_doc;
        del_doc.property(config_.docid_property_name) = from_uuid;
        op_processor_->Append(3, del_doc);
    }
    else
    {
        PMDocumentType diff_properties;
        ProductPrice price;
        GetPrice_(docid_list, price);
        diff_properties.property(config_.price_property_name) = price.ToUString();
        diff_properties.property(config_.docid_property_name) = from_uuid;
        SetItemCount_(diff_properties, docid_list.size());
        op_processor_->Append(2, diff_properties);
    }
    return true;
}

bool ProductManager::FinishHook()
{
    if (has_price_trend_)
    {
        price_trend_->Flush();
    }

    if(clustering_!=NULL)
    {
        uint32_t max_in_group = 100;
        if(!clustering_->Run())
        {
            std::cout<<"ProductClustering Run failed"<<std::endl;
            return false;
        }
        typedef ProductClustering::GroupTableType GroupTableType;
        GroupTableType* group_table = clustering_->GetGroupTable();
        typedef izenelib::util::UString UuidType;
//         boost::unordered_map<GroupTableType::GroupIdType, UuidType> g2u_map;
        boost::unordered_map<GroupTableType::GroupIdType, PMDocumentType> g2doc_map;

        //output DOCID -> uuid map SCD
        ScdWriter* uuid_map_writer = NULL;
        if(!config_.uuid_map_path.empty() )
        {
            boost::filesystem::create_directories(config_.uuid_map_path);
            uuid_map_writer = new ScdWriter(config_.uuid_map_path, INSERT_SCD);
        }
        const std::vector<std::vector<GroupTableType::DocIdType> >& group_info = group_table->GetGroupInfo();
        LOG(INFO)<<"Start building group info."<<std::endl;
        for(uint32_t gid = 0;gid<group_info.size();gid++)
        {
            std::vector<GroupTableType::DocIdType> in_group = group_info[gid];
            if(in_group.size()<2) continue;
            if(max_in_group >0 && in_group.size()>max_in_group) continue;
            std::sort(in_group.begin(), in_group.end());
            //use the smallest docid as uuid
            izenelib::util::UString docname(in_group[0], izenelib::util::UString::UTF_8);
            UuidType uuid;
            generateUUID(docname, uuid);

            PMDocumentType doc;
            doc.property(config_.docid_property_name) = docname;
            doc.property(config_.price_property_name) = izenelib::util::UString("", izenelib::util::UString::UTF_8);
            doc.property(config_.uuid_property_name) = uuid;
            SetItemCount_(doc, in_group.size());
            g2doc_map.insert(std::make_pair(gid, doc));
            if(uuid_map_writer!=NULL)
            {
                for(uint32_t i=0;i<in_group.size();i++)
                {
                    PMDocumentType map_doc;
                    map_doc.property(config_.docid_property_name) = izenelib::util::UString(in_group[i], izenelib::util::UString::UTF_8);
                    map_doc.property(config_.uuid_property_name) = uuid;
                    uuid_map_writer->Append(map_doc);
                }
            }
        }
        if(uuid_map_writer!=NULL)
        {
            uuid_map_writer->Close();
            delete uuid_map_writer;
        }
        LOG(INFO)<<"Finished building group info."<<std::endl;
        std::vector<std::pair<uint32_t, izenelib::util::UString> > uuid_update_list;
        uint32_t append_count = 0;
//         uint32_t has_comparison_count = 0;
#ifdef PM_PROFILER
        static double t1=0.0;
        static double t2=0.0;
        static double t3=0.0;
        static double t4=0.0;
//         std::cout<<"start buffer size : "<<common_compressed_.bufferCapacity()<<std::endl;
        izenelib::util::ClockTimer timer;
#endif
        uuid_update_list.reserve(data_source_->GetMaxDocId());
        for(uint32_t docid=1; docid <= data_source_->GetMaxDocId(); ++docid )
        {
            if(docid%100000==0)
            {
                LOG(INFO)<<"Process "<<docid<<" docs"<<std::endl;
            }
#ifdef PM_PROFILER
            if(docid%10 == 0)
            {
                LOG(INFO)<<"PM_PROFILER : ["<<has_comparison_count<<"] "<<t1<<","<<t2<<","<<t3<<","<<t4<<std::endl;
            }
#endif
            PMDocumentType doc;
#ifdef PM_PROFILER
            timer.restart();
#endif
            if(!data_source_->GetDocument(docid, doc) )
            {
                continue;
            }
#ifdef PM_PROFILER
            t1 += timer.elapsed();
#endif
            UString udocid;
            std::string sdocid;
            GetDOCID_(doc, udocid);
            ProductPrice price;
            GetPrice_(doc, price);
            udocid.convertString(sdocid, izenelib::util::UString::UTF_8);
            GroupTableType::GroupIdType group_id;
            bool in_group = false;
            if(group_table->GetGroupId(sdocid, group_id))
            {
                if(g2doc_map.find(group_id)!=g2doc_map.end())
                {
                    in_group = true;
                }
            }

//             uint32_t itemcount = 1;
//             std::vector<std::string> docid_list_in_group;
//             bool append = true;
            if(in_group )
            {
//                 boost::unordered_map<GroupTableType::GroupIdType, UuidType>::iterator g2u_it = g2u_map.find(group_id);
                boost::unordered_map<GroupTableType::GroupIdType, PMDocumentType>::iterator g2doc_it = g2doc_map.find(group_id);
                PMDocumentType& combine_doc = g2doc_it->second;
                ProductPrice combine_price;
                GetPrice_(combine_doc, combine_price);
                combine_price += price;
                izenelib::util::UString base_udocid = doc.property(config_.docid_property_name).get<izenelib::util::UString>();
                UString uuid = combine_doc.property(config_.uuid_property_name).get<izenelib::util::UString>();
                if(udocid == base_udocid )
                {
                    combine_doc.copyPropertiesFromDocument(doc, false);
                    combine_doc.property(config_.price_property_name) = combine_price.ToUString();
                }
                else
                {
                    combine_doc.property(config_.price_property_name) = combine_price.ToUString();
                }
                uuid_update_list.push_back(std::make_pair(docid, uuid));
            }
            else
            {
                UString uuid;
                //not in any group
                generateUUID(udocid, uuid);
                doc.property(config_.uuid_property_name) = uuid;
                uuid_update_list.push_back(std::make_pair(docid, uuid));
                PMDocumentType new_doc(doc);
                new_doc.property(config_.docid_property_name) = uuid;
                new_doc.eraseProperty(config_.uuid_property_name);
                SetItemCount_(new_doc, 1);
                op_processor_->Append(1, new_doc);
                ++append_count;
            }
        }
        //process the comparison items.
        boost::unordered_map<GroupTableType::GroupIdType, PMDocumentType>::iterator g2doc_it = g2doc_map.begin();
        while( g2doc_it!= g2doc_map.end())
        {
            PMDocumentType& doc = g2doc_it->second;
            PMDocumentType new_doc(doc);
            new_doc.property(config_.docid_property_name) = new_doc.property(config_.uuid_property_name);
            new_doc.eraseProperty(config_.uuid_property_name);
            op_processor_->Append(1, new_doc);
            ++append_count;
            ++g2doc_it;
        }

        //process update_list
        LOG(INFO)<<"Total update list count : "<<uuid_update_list.size()<<std::endl;
        for(uint32_t i=0;i<uuid_update_list.size();i++)
        {
            if(i%100000==0)
            {
                LOG(INFO)<<"Updated "<<i<<std::endl;
            }
            std::vector<uint32_t> update_docid_list(1, uuid_update_list[i].first);
            if(!data_source_->UpdateUuid(update_docid_list, uuid_update_list[i].second) )
            {
                LOG(INFO)<<"UpdateUuid fail for docid : "<<uuid_update_list[i].first<<" | "<<uuid_update_list[i].second<<std::endl;
            }
        }
        data_source_->Flush();
        LOG(INFO)<<"Generate "<<append_count<<" docs for b5ma"<<std::endl;
        delete clustering_;
        clustering_ = NULL;
    }

    return GenOperations_();
}

bool ProductManager::UpdateADoc(const Document& doc, bool backup)
{
    if (!UpdateADoc_(doc))
    {
        error_ = "Update A Doc failed";
        return false;
    }
    if (!GenOperations_())
    {
        return false;
    }
    if (backup && backup_)
    {
        backup_->AddProductUpdateItem(doc);
    }
    return true;
}

bool ProductManager::UpdateADoc_(const Document& doc)
{
    op_processor_->Append(2, doc);
    return true;
}

bool ProductManager::AddGroupWithInfo(const std::vector<UString>& docid_list, const Document& doc, bool backup)
{
    std::vector<uint32_t> idocid_list;
    if (!data_source_->GetInternalDocidList(docid_list, idocid_list))
    {
        error_ = data_source_->GetLastError();
        return false;
    }
    return AddGroupWithInfo(idocid_list, doc, backup);
}

void ProductManager::BackupPCItem_(const UString& uuid, const std::vector<uint32_t>& docid_list, int type)
{
    std::vector<UString> docname_list;
    docname_list.reserve(docid_list.size());
    for (uint32_t i = 0; i < docid_list.size(); i++)
    {
        PMDocumentType doc;
        if (data_source_->GetDocument(docid_list[i], doc))
        {
            docname_list.push_back(doc.property(config_.docid_property_name).get<UString>());
        }
    }
    if (!backup_->AddPriceComparisonItem(uuid, docname_list, type))
    {
        std::cout<<"backup failed"<<std::endl;
    }
}

bool ProductManager::CheckAddGroupWithInfo(const std::vector<izenelib::util::UString>& sdocid_list, const Document& doc)
{
    std::vector<uint32_t> docid_list;
    if (!data_source_->GetInternalDocidList(sdocid_list, docid_list))
    {
        error_ = data_source_->GetLastError();
        return false;
    }
    UString uuid = doc.property(config_.docid_property_name).get<UString>();
    if (uuid.length()==0)
    {
        error_ = "DOCID(uuid) not set";
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if (!uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, UString::UTF_8);
        error_ = suuid+" already exists";
        return false;
    }

    std::vector<PMDocumentType> doc_list(docid_list.size());
    for (uint32_t i = 0; i < docid_list.size(); i++)
    {
        if (!data_source_->GetDocument(docid_list[i], doc_list[i]))
        {
            error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
    }
    std::vector<UString> uuid_list(doc_list.size());
    for (uint32_t i = 0; i < doc_list.size(); i++)
    {
        if (!GetUuid_(doc_list[i], uuid_list[i]))
        {
            error_ = "Can not get uuid in document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
        std::vector<uint32_t> same_docid_list;
        data_source_->GetDocIdList(uuid_list[i], same_docid_list, docid_list[i]);
        if (!same_docid_list.empty())
        {
            error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[i])+" belongs to other group";
            return false;
        }
        if (uuid_list[i] == uuid)
        {
            error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[i])+" has the same uuid with request";
            return false;
        }
    }
    return true;
}

bool ProductManager::AddGroupWithInfo(const std::vector<uint32_t>& docid_list, const Document& doc, bool backup)
{
    if (inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::AddGroupWithInfo"<<std::endl;
    UString uuid = doc.property(config_.docid_property_name).get<UString>();
    if (uuid.length()==0)
    {
        error_ = "DOCID(uuid) not set";
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if (!uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, UString::UTF_8);
        error_ = suuid+" already exists";
        return false;
    }
    //call updateA

    if (!AppendToGroup_(uuid, uuid_docid_list, docid_list, doc))
    {
        return false;
    }

//     if (!GenOperations_())
//     {
//         return false;
//     }
    if (backup && backup_)
    {
        BackupPCItem_(uuid, docid_list, 1);
        backup_->AddProductUpdateItem(doc);
    }
    return true;
}

bool ProductManager::AddGroup(const std::vector<uint32_t>& docid_list, UString& gen_uuid, bool backup)
{
    if (inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::AddGroup"<<std::endl;
    if (docid_list.size()<2)
    {
        error_ = "Docid list size must larger than 1";
        return false;
    }
    PMDocumentType first_doc;
    if (!data_source_->GetDocument(docid_list[0], first_doc))
    {
        error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[0]);
        return false;
    }
    UString first_uuid;
    if (!GetUuid_(first_doc, first_uuid))
    {
        error_ = "Can not get uuid in document "+boost::lexical_cast<std::string>(docid_list[0]);
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(first_uuid, uuid_docid_list, 0);
    if (uuid_docid_list.empty())
    {
        std::string suuid;
        first_uuid.convertString(suuid, UString::UTF_8);
        error_ = suuid+" not exists";
        return false;
    }
    if (uuid_docid_list.size()>1 || uuid_docid_list[0]!= docid_list[0])
    {
        error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[0])+" belongs to other group";
        return false;
    }

    std::vector<uint32_t> remain(docid_list.begin()+1, docid_list.end());
    if (!AppendToGroup_(first_uuid, uuid_docid_list, remain, PMDocumentType()))
    {
        return false;
    }
    if (!GenOperations_())
    {
        return false;
    }
    if (backup && backup_)
    {
        BackupPCItem_(first_uuid, docid_list, 1);
    }
    gen_uuid = first_uuid;
    return true;
}

bool ProductManager::GenOperations_()
{
    bool result = true;
    if (!op_processor_->Finish())
    {
        error_ = op_processor_->GetLastError();
        result = false;
    }
    if (inhook_) inhook_ = false;
    return result;

    /// M
    /*
    void ProductManager::onProcessed(bool success)
    {
        // action after SCDs have been processed
    }

    DistributedProcessSynchronizer dsSyn;
    std::string scdDir = "/tmp/hdfs/scd"; // generated scd files in scdDir
    dsSyn.generated(scdDir);
    dsSyn.watchProcess(boost::bind(&ProductManager::onProcessed, this,_1));
    */

    /// A
    /*
    class Receiver {
    public:
        Receiver()
        {
            dsSyn.watchGenerate(boost::bind(&Receiver::onGenerated, this,_1));
        }

        void Receiver::onGenerated(const std::string& s)
        {
            // process SCDs

            dsSyn.processed(true);
        }

        DistributedProcessSynchronizer dsSyn;
    }
    */
}

bool ProductManager::AppendToGroup_(const UString& uuid, const std::vector<uint32_t>& uuid_docid_list, const std::vector<uint32_t>& docid_list, const PMDocumentType& uuid_doc)
{
    if (docid_list.empty())
    {
        error_ = "Docid list size must larger than 0";
        return false;
    }

    std::vector<PMDocumentType> doc_list(docid_list.size());
    for (uint32_t i = 0; i < docid_list.size(); i++)
    {
        if (!data_source_->GetDocument(docid_list[i], doc_list[i]))
        {
            error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
    }
    std::vector<UString> uuid_list(doc_list.size());
    for (uint32_t i = 0; i < doc_list.size(); i++)
    {
        if (!GetUuid_(doc_list[i], uuid_list[i]))
        {
            error_ = "Can not get uuid in document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
        std::vector<uint32_t> same_docid_list;
        data_source_->GetDocIdList(uuid_list[i], same_docid_list, docid_list[i]);
        if (!same_docid_list.empty())
        {
            error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[i])+" belongs to other group";
            return false;
        }
        if (uuid_list[i] == uuid)
        {
            error_ = "Document id "+boost::lexical_cast<std::string>(docid_list[i])+" has the same uuid with request";
            return false;
        }
    }

    std::vector<uint32_t> all_docid_list(uuid_docid_list);
    all_docid_list.insert(all_docid_list.end(), docid_list.begin(), docid_list.end());
    ProductPrice price;
    GetPrice_(all_docid_list, price);
//     std::cout<<"validation finished here."<<std::endl;
    //validation finished here.

    //commit firstly, then update DM and IM
    for (uint32_t i = 0; i < uuid_list.size(); i++)
    {
        PMDocumentType del_doc;
        del_doc.property(config_.docid_property_name) = uuid_list[i];
        op_processor_->Append(3, del_doc);
    }
    if (uuid_docid_list.empty())
    {
        //this uuid is a new, use the first doc as base;
        PMDocumentType output(doc_list[0]);
        PMDocumentType::property_const_iterator uit = uuid_doc.propertyBegin();
        while (uit != uuid_doc.propertyEnd())
        {
            output.property(uit->first) = uit->second;
            ++uit;
        }
        output.property(config_.docid_property_name) = uuid;
        SetItemCount_(output, all_docid_list.size());
        output.property(config_.price_property_name) = price.ToUString();
        output.eraseProperty(config_.uuid_property_name);
        op_processor_->Append(1, output);
    }
    else
    {
        PMDocumentType output;
        if (uuid_doc.hasProperty(config_.docid_property_name))
        {
            output = uuid_doc;
        }
        output.property(config_.docid_property_name) = uuid;
        SetItemCount_(output, all_docid_list.size());
        output.property(config_.price_property_name) = price.ToUString();
        op_processor_->Append(2, output);
    }

    //update DM and IM then
    if (!data_source_->UpdateUuid(docid_list, uuid))
    {
        error_ = "Update uuid failed";
        return false;
    }
    data_source_->Flush();
    return true;
}

bool ProductManager::AppendToGroup(const UString& uuid, const std::vector<uint32_t>& docid_list, bool backup)
{
    if (inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::AppendToGroup"<<std::endl;
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if (uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, UString::UTF_8);
        error_ = suuid+" not exists";
        return false;
    }
    if (!AppendToGroup_(uuid, uuid_docid_list, docid_list, PMDocumentType()))
    {
        return false;
    }
    if (!GenOperations_())
    {
        return false;
    }
    if (backup && backup_)
    {
        BackupPCItem_(uuid, docid_list, 1);
    }
    return true;
}

bool ProductManager::RemoveFromGroup(const UString& uuid, const std::vector<uint32_t>& docid_list, bool backup)
{
    if (inhook_)
    {
        error_ = "In Hook locks, collection was indexing, plz wait.";
        return false;
    }
    boost::mutex::scoped_lock lock(human_mutex_);
    std::cout<<"ProductManager::RemoveFromGroup"<<std::endl;
    if (docid_list.empty())
    {
        error_ = "Docid list size must larger than 0";
        return false;
    }
    std::vector<uint32_t> uuid_docid_list;
    data_source_->GetDocIdList(uuid, uuid_docid_list, 0);
    if (uuid_docid_list.empty())
    {
        std::string suuid;
        uuid.convertString(suuid, UString::UTF_8);
        error_ = suuid+" not exists";
        return false;
    }
    boost::unordered_set<uint32_t> contains;
    for (uint32_t i = 0; i < uuid_docid_list.size(); i++)
    {
        contains.insert(uuid_docid_list[i]);
    }
    for (uint32_t i = 0; i < docid_list.size(); i++)
    {
        if (contains.find(docid_list[i]) == contains.end())
        {
            error_ = "Document "+boost::lexical_cast<std::string>(docid_list[i])+" not in specific uuid";
            return false;
        }
        contains.erase(docid_list[i]);
    }
    std::vector<uint32_t> remain(contains.begin(), contains.end());
    std::vector<PMDocumentType> doc_list(docid_list.size());
    for (uint32_t i = 0; i < docid_list.size(); i++)
    {
        if (!data_source_->GetDocument(docid_list[i], doc_list[i]))
        {
            error_ = "Can not get document "+boost::lexical_cast<std::string>(docid_list[i]);
            return false;
        }
    }

    if (remain.empty())
    {
        PMDocumentType del_doc;
        del_doc.property(config_.docid_property_name) = uuid;
        op_processor_->Append(3, del_doc);
    }
    else
    {
        ProductPrice price;
        GetPrice_(remain, price);
        //validation finished here.

        PMDocumentType update_doc;
        update_doc.property(config_.docid_property_name) = uuid;
        SetItemCount_(update_doc, remain.size());
        update_doc.property(config_.price_property_name) = price.ToUString();
        op_processor_->Append(2, update_doc);
    }
    std::vector<UString> uuid_list(doc_list.size());
    for (uint32_t i = 0; i < doc_list.size(); i++)
    {
        //TODO need to be more strong here.
        generateUUID(uuid_list[i], doc_list[i]);
        doc_list[i].property(config_.docid_property_name) = uuid_list[i];
        SetItemCount_(doc_list[i], doc_list.size());
        doc_list[i].eraseProperty(config_.uuid_property_name);
        op_processor_->Append(1, doc_list[i]);
    }
    if (!GenOperations_())
    {
        return false;
    }
    if (backup && backup_)
    {
        BackupPCItem_(uuid, docid_list, 2);
    }
    //update DM and IM here
    for (uint32_t i=0; i < docid_list.size(); i++)
    {
        std::vector<uint32_t> tmp_list(1, docid_list[i]);
        if (!data_source_->UpdateUuid(tmp_list, uuid_list[i]))
        {
            //TODO how to rollback?
        }
    }
    data_source_->Flush();
    return true;
}

bool ProductManager::GetMultiPriceHistory(
        PriceHistoryList& history_list,
        const std::vector<std::string>& docid_list,
        time_t from_tt,
        time_t to_tt)
{
    if (!has_price_trend_)
    {
        error_ = "Price trend is not enabled for this collection";
        return false;
    }

    return price_trend_->GetMultiPriceHistory(history_list, docid_list, from_tt, to_tt, error_);
}

bool ProductManager::GetMultiPriceRange(
        PriceRangeList& range_list,
        const std::vector<std::string>& docid_list,
        time_t from_tt,
        time_t to_tt)
{
    if (!has_price_trend_)
    {
        error_ = "Price trend is not enabled for this collection";
        return false;
    }

    return price_trend_->GetMultiPriceRange(range_list, docid_list, from_tt, to_tt, error_);
}

bool ProductManager::GetTopPriceCutList(
        TPCQueue& tpc_queue,
        const std::string& prop_name,
        const std::string& prop_value,
        uint32_t days,
        uint32_t count)
{
    if (!has_price_trend_)
    {
        error_ = "Price trend is not enabled for this collection";
        return false;
    }

    return price_trend_->GetTopPriceCutList(tpc_queue, prop_name, prop_value, days, count, error_);
}

bool ProductManager::GetPrice_(uint32_t docid, ProductPrice& price) const
{
    PMDocumentType doc;
    if (!data_source_->GetDocument(docid, doc)) return false;
    return GetPrice_(doc, price);
}

bool ProductManager::GetPrice_(const PMDocumentType& doc, ProductPrice& price) const
{
    Document::property_const_iterator it = doc.findProperty(config_.price_property_name);
    if (it == doc.propertyEnd())
    {
        return false;
    }
    const UString& price_str = it->second.get<UString>();
    return price.Parse(price_str);
}

void ProductManager::GetPrice_(const std::vector<uint32_t>& docid_list, ProductPrice& price) const
{
    for (uint32_t i = 0; i < docid_list.size(); i++)
    {
        ProductPrice p;
        if (GetPrice_(docid_list[i], p))
        {
            price += p;
        }
    }
}

bool ProductManager::GetUuid_(const PMDocumentType& doc, UString& uuid) const
{
    PMDocumentType::property_const_iterator it = doc.findProperty(config_.uuid_property_name);
    if (it == doc.propertyEnd())
    {
        return false;
    }
    uuid = it->second.get<UString>();
    return true;
}

bool ProductManager::GetDOCID_(const PMDocumentType& doc, UString& docid) const
{
    PMDocumentType::property_const_iterator it = doc.findProperty(config_.docid_property_name);
    if (it == doc.propertyEnd())
    {
        return false;
    }
    docid = it->second.get<UString>();
    return true;
}

bool ProductManager::GetCategory_(const PMDocumentType& doc, izenelib::util::UString& category)
{
    return doc.getProperty(config_.category_property_name, category);
}

bool ProductManager::GetTimestamp_(const PMDocumentType& doc, time_t& timestamp) const
{
    PMDocumentType::property_const_iterator it = doc.findProperty(config_.date_property_name);
    if (it == doc.propertyEnd())
    {
        return false;
    }
    const UString& time_ustr = it->second.get<UString>();
    std::string time_str;
    time_ustr.convertString(time_str, UString::UTF_8);
    try
    {
        timestamp = Utilities::createTimeStamp(boost::posix_time::from_iso_string(time_str.insert(8, 1, 'T')));
    }
    catch (const std::exception& ex)
    {
        return false;
    }
    return true;
}

bool ProductManager::GetGroupProperties_(const PMDocumentType& doc, std::map<std::string, std::string>& group_prop_map) const
{
    for (uint32_t i = 0; i < config_.group_property_names.size(); i++)
    {
        Document::property_const_iterator it = doc.findProperty(config_.group_property_names[i]);
        if (it == doc.propertyEnd()) continue;
        UString prop_ustr = it->second.get<UString>();
        std::string& prop_str = group_prop_map[config_.group_property_names[i]];
        prop_ustr.convertString(prop_str, UString::UTF_8);
        size_t pos;
        if ((pos = prop_str.find('>')) != std::string::npos)
            prop_str.resize(pos);
    }
    return group_prop_map.empty();
}

void ProductManager::SetItemCount_(PMDocumentType& doc, uint32_t item_count)
{
    doc.property(config_.itemcount_property_name) = UString(boost::lexical_cast<std::string>(item_count), UString::UTF_8);
}
