#include "property_diversity_reranker.h"

#include <mining-manager/group-label-logger/GroupLabelLogger.h>

#include <list>
#include <algorithm>
#include <glog/logging.h>

using sf1r::GroupLabelLogger;

NS_FACETED_BEGIN


bool CompareSecond(const std::pair<size_t, count_t>& r1, const std::pair<size_t, count_t>& r2)
{
    return (r1.second > r2.second);
}

PropertyDiversityReranker::PropertyDiversityReranker(
    const std::string& property,
    const GroupManager::PropValueMap& propValueMap,
    const std::string& boostingProperty
)
    :property_(property)
    ,propValueMap_(propValueMap)
    ,groupLabelLogger_(NULL)
    ,boostingProperty_(boostingProperty)
{
}

PropertyDiversityReranker::~PropertyDiversityReranker()
{
}

void PropertyDiversityReranker::simplererank(
    std::vector<unsigned int>& docIdList,
    std::vector<float>& rankScoreList
)
{
    std::size_t numDoc = docIdList.size();
    std::vector<unsigned int> newDocIdList;
    std::vector<float> newScoreList;
    newDocIdList.reserve(numDoc);
    newScoreList.reserve(numDoc);

    // rerank by group
    typedef std::list<std::pair<unsigned int,float> > DocIdList;
    typedef std::map<PropValueTable::pvid_t, DocIdList> DocIdMap;

    bool successGroup = true;
    GroupManager::PropValueMap::const_iterator pvIt = propValueMap_.find(property_);
    if (pvIt == propValueMap_.end())
    {
        LOG(ERROR) << "in GroupManager: group index file is not loaded for group property " << property_;
        successGroup = false;
    }
    else
    {
        const PropValueTable& pvTable = pvIt->second;
        const PropValueTable::ValueIdTable& idTable = pvTable.valueIdTable();

        DocIdMap docIdMap;
        DocIdList missDocs;
        for (std::size_t i = 0; i < numDoc; ++i)
        {
            docid_t docId = docIdList[i];
            
            // this doc has not built group index data
            if (docId >= idTable.size())
                continue;
            
            const PropValueTable::ValueIdList& valueIdList = idTable[docId];
            if (valueIdList.empty())
            {
                missDocs.push_back(std::make_pair(docId, rankScoreList[i]));
            }
            else
            {
                // use 1st group value
                PropValueTable::pvid_t pvId = valueIdList[0];
                docIdMap[pvId].push_back(std::make_pair(docId, rankScoreList[i]));
            }
        }

        if(docIdMap.size() <= 1) // single property or empty
            successGroup = false;
        else
        {
            do{
                DocIdMap::iterator mapIt = docIdMap.begin();
                while(mapIt != docIdMap.end())
                {
                    if(mapIt->second.empty())
                    {
                        docIdMap.erase(mapIt++);
                    }
                    else
                    {
                        std::pair<unsigned int, float> element = mapIt->second.front();
                        mapIt->second.pop_front();
                        newDocIdList.push_back(element.first);
                        newScoreList.push_back(element.second);
                        ++mapIt;
                    }
                }
            }while(!docIdMap.empty());

            for(DocIdList::iterator missIt = missDocs.begin(); missIt != missDocs.end(); ++missIt)
            {
                newDocIdList.push_back(missIt->first);
                newScoreList.push_back(missIt->second);
            }
        }
    }

    // boosting by CTR
    std::vector<std::pair<size_t, count_t> > posClickCountList;
    if (ctrManager_)
    {
        if (successGroup)
            ctrManager_->getClickCountListByDocIdList(newDocIdList, posClickCountList);
        else
            ctrManager_->getClickCountListByDocIdList(docIdList, posClickCountList);
    }

    size_t boostDocNum = posClickCountList.size();
    if (boostDocNum > 0)
    {
        if (!successGroup)
        {
            for (size_t i = 0; i < numDoc; i++)
            {
                newDocIdList.push_back(docIdList[i]);
                newScoreList.push_back(rankScoreList[i]);
            }
        }

        // sort by click-count
        std::sort(posClickCountList.begin(), posClickCountList.end(), CompareSecond);

        std::vector<std::pair<size_t, count_t> >::iterator it;
        size_t i = 0;
        for (it = posClickCountList.begin(); it != posClickCountList.end(); it++)
        {
            size_t& pos = it->first;
            docIdList[i] = newDocIdList[pos];
            rankScoreList[i] = newScoreList[pos];

            newDocIdList[pos] = 0;
            i++;
        }

        for (size_t pos = 0; pos < newDocIdList.size(); pos++)
        {
            if (newDocIdList[pos] != 0)
            {
                docIdList[i] = newDocIdList[pos];
                rankScoreList[i] = newScoreList[pos];
                i++;
            }
        }
    }
    else
    {
        if (successGroup)
        {
            using std::swap;
            swap(docIdList, newDocIdList);
            swap(rankScoreList, newScoreList);
        }
    }
}

void PropertyDiversityReranker::rerank(
    std::vector<unsigned int>& docIdList,
    std::vector<float>& rankScoreList,
    const std::string& query
)
{
    typedef std::list<std::pair<unsigned int,float> > DocIdList;
    typedef std::map<PropValueTable::pvid_t, DocIdList> DocIdMap;

    std::vector<std::string> propValueVec;
    std::vector<int> freqVec;
    if(groupLabelLogger_)
    {
        groupLabelLogger_->getFreqLabel(query, 1, propValueVec, freqVec);
    }

    if(!propValueVec.empty())
    {
        std::string& boostingCategoryLabel = propValueVec[0];
        std::cout<<"boosting category "<<boostingCategoryLabel<<std::endl;		
        GroupManager::PropValueMap::const_iterator pvIt = propValueMap_.find(boostingProperty_);
        if (pvIt == propValueMap_.end())
        {
            LOG(ERROR) << "in GroupManager: group index file is not loaded for group property " << boostingProperty_;
            simplererank(docIdList, rankScoreList);
            return;
        }
        const PropValueTable& pvTable = pvIt->second;
        izenelib::util::UString labelUStr(boostingCategoryLabel, izenelib::util::UString::UTF_8);
        PropValueTable::pvid_t labelId = pvTable.propValueId(labelUStr);
        if (labelId == 0)
        {
            LOG(WARNING) << "in GroupManager: group index has not been built for Category value: " << boostingCategoryLabel;
            simplererank(docIdList, rankScoreList);			
            return;
        }

        std::size_t numDoc = docIdList.size();
		
        std::vector<unsigned int> boostingDocIdList;
        std::vector<float> boostingScoreList;	
        boostingDocIdList.reserve(numDoc);;
        boostingScoreList.reserve(numDoc);

        std::vector<unsigned int> leftDocIdList;
        std::vector<float> leftScoreList;	
        leftDocIdList.reserve(numDoc);;
        leftScoreList.reserve(numDoc);
		
        for(std::size_t i = 0; i < numDoc; ++i)
        {
            docid_t docId = docIdList[i];

            if (pvTable.testDoc(docId, labelId))
            {
                boostingDocIdList.push_back(docId);
                boostingScoreList.push_back(rankScoreList[i]);
            }
            else
            {
                leftDocIdList.push_back(docId);
                leftScoreList.push_back(rankScoreList[i]);
            }
        }
        if(!boostingDocIdList.empty())
        {
            simplererank(boostingDocIdList, boostingScoreList);
        }
        if(!leftDocIdList.empty())
        {
            simplererank(leftDocIdList, leftScoreList);
        }
        boostingDocIdList.resize(numDoc);
        boostingScoreList.resize(numDoc);
        std::copy_backward(leftDocIdList.begin(), leftDocIdList.end(), boostingDocIdList.end());
        std::copy_backward(leftScoreList.begin(), leftScoreList.end(), boostingScoreList.end());
        using std::swap;
        swap(docIdList, boostingDocIdList);
        swap(rankScoreList, boostingScoreList);
    }
    else
        simplererank(docIdList, rankScoreList);
}

NS_FACETED_END

