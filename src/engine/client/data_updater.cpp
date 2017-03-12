#include <base/system.h>
#include <openssl/sha.h>
#include <engine/storage.h>
#include <base/system++/io.h>
#include <engine/shared/config.h>
#include <json-parser/json.hpp>
#include <game/version.h>
#include "data_updater.h"
#include "updater.h"
#include "curlwrapper.h"


using std::map;
using std::string;


CGitHubAPI::CGitHubAPI()
{
	if((m_pHandle = curl_easy_init()) == NULL)
	{
		dbg_msg("github", "failed to create curl easy handle");
		m_State = STATE_ERROR;
	}
	else
		m_State = STATE_CLEAN;

	mem_zerob(m_aLatestVersion);
	m_aLatestVersion[0] = '0';

	m_DownloadJobs.clear();
	m_RemoveJobs.clear();
	m_RenameJobs.clear();
}

CGitHubAPI::~CGitHubAPI()
{
	if(m_pHandle)
		curl_easy_cleanup(m_pHandle);
}

void CGitHubAPI::CheckVersion()
{
	// determine whether we may actually check again yet - because github limits us 60 requests per hour
//	IOHANDLE_SMART f = Storage()->blabla("tmp/cache/versionrefreshtime");
//	int64 LastSecond;
//	int64 CurrSecond = time_timestamp();
//	if(CurrSecond < LastSecond+60)
//	{
//		dbg_msg("github", "refresh version info will be available again in %u seconds", 60-CurrSecond-LastSecond);
//		return;
//	}

	dbg_msg("github", "refreshing version info");
	m_State = STATE_REFRESHING;

	THREAD_SMART<CGitHubAPI> Thread(CGitHubAPI::UpdateCheckerThread);
	if(!Thread.Start(this))
	{
		dbg_msg("github", "ERROR: failed to start UpdateChecker-thread");
		m_State = STATE_ERROR;
	}
}

void CGitHubAPI::DoUpdate()
{
	dbg_msg("github", "performing data update");
	m_State = STATE_COMPARING;

	THREAD_SMART<CGitHubAPI> Thread(CGitHubAPI::CompareThread);
	if(!Thread.Start(this))
	{
		dbg_msg("github", "ERROR: failed to start Compare-thread");
		m_State = STATE_ERROR;
	}
}

//---------------------------- STEP 1: UPDATE CHECKING ----------------------------//

void CGitHubAPI::UpdateCheckerThread(CGitHubAPI *pSelf)
{
	std::string Result;
	char aUrl[512];
	str_copyb(aUrl, GITHUB_API_URL "/releases?page=1&per_page=1");
	Result = pSelf->SimpleGET(aUrl);
	if(Result.empty() || Result.length() == 0)
	{
		dbg_msg("github", "ERROR: failed to download version info");
		pSelf->m_State = STATE_ERROR;
		return;
	}

	// check if we have the latest version
	{
		const char *pLatestVersion = ParseReleases(Result.c_str()).c_str();
		if(str_length(pLatestVersion) == 0)
		{
			dbg_msg("github", "ERROR: failed to parse out the latest version");
			pSelf->m_State = STATE_ERROR;
			return;
		}
		else
		{
			pSelf->m_State = STATE_CLEAN;
			if(str_comp_nocase(pLatestVersion, GAME_ATH_VERSION) == 0)
			{
				dbg_msg("github", "AllTheHaxx is up to date.");
				return;
			}
			else
			{
				pSelf->m_State = STATE_NEW_VERSION;
				str_copyb(pSelf->m_aLatestVersion, pLatestVersion);
				dbg_msg("github", " -- NEW VERSION: AllTheHaxx %s has been released! --", pLatestVersion);
				return;
			}
		}
	}
}

const std::string CGitHubAPI::ParseReleases(const char *pJsonStr)
{
	// gets json[0]["name"]

	json_value &jsonVersions = *json_parse(pJsonStr, (size_t)str_length(pJsonStr));
	const json_value &jsonName = jsonVersions[0]["name"];

	std::string Result((const char *)jsonName);

	json_value_free(&jsonVersions);

	return Result;
}

//---------------------------- STEP 2: CHANGELIST CREATION ----------------------------//

void CGitHubAPI::CompareThread(CGitHubAPI *pSelf)
{
	if(str_comp(pSelf->m_aLatestVersion, "0") == 0)
	{
		dbg_msg("github", "WTF: compare thread started but got no version info?!");
		pSelf->m_State = STATE_ERROR;
		return;
	}

	std::string Result;
	char aUrl[512];
	str_formatb(aUrl, GITHUB_API_URL "/compare/%s...%s", GAME_ATH_VERSION, pSelf->m_aLatestVersion);
	Result = pSelf->SimpleGET(aUrl);
	if(Result.empty() || Result.length() == 0)
	{
		dbg_msg("github/compare", "ERROR: result empty");
		pSelf->m_State = STATE_ERROR;
		return;
	}

	if(!pSelf->ParseCompare(Result.c_str()))
	{
		pSelf->m_State = STATE_ERROR;
		dbg_msg("github/compare", "ERROR: parsing failed");
	}
	else
	{
		pSelf->m_State = STATE_DONE;
		int NumDownload = (int)pSelf->m_DownloadJobs.size();
		int NumRename = (int)pSelf->m_RenameJobs.size();
		int NumRemove = (int)pSelf->m_RemoveJobs.size();
		dbg_msg("github/compare", "got %i jobs; download=%i, rename=%i, delete=%i",
				NumDownload + NumRename + NumRemove,
				NumDownload, NumRename, NumRemove
		);

		for(int i = 0; i < NumDownload; i++)
			dbg_msg("github", "JOBS:DL:%03i> '%s'", i, pSelf->m_DownloadJobs[i].c_str());
		for(int i = 0; i < NumRename; i++)
			dbg_msg("github", "JOBS:MV:%03i> '%s' -> '%s'", i, pSelf->m_RenameJobs[i].first.c_str(), pSelf->m_RenameJobs[i].second.c_str());
		for(int i = 0; i < NumRemove; i++)
			dbg_msg("github", "JOBS:RM:%03i> '%s'", i, pSelf->m_RemoveJobs[i].c_str());

	}
}



bool CGitHubAPI::ParseCompare(const char *pJsonStr)
{
	// gets json["files"][i]["filename"]

	json_value &jsonCompare = *json_parse(pJsonStr, (size_t)str_length(pJsonStr));

	const json_value &jsonFiles = jsonCompare["files"];
	if(jsonFiles.type != json_array)
		return false;

	int NumCommits = jsonCompare["commits"].u.array.length;
	str_copyb(m_aLatestVersionTree, jsonCompare["commits"][NumCommits-1]["sha"]); // the last commit describes the tree we want
	dbg_msg("github", "latest version tree is '%s'; there were %i commits since our version", m_aLatestVersionTree, NumCommits);

	// loop through the array of changed files
	for(unsigned int i = 0; i < jsonFiles.u.array.length; i++)
	{
		// we only want files in the data/ directory
		const char *pFilename = (const char *)(jsonFiles[i]["filename"]);
		if(str_comp_nocase_num(pFilename, "data/", str_length("data/")) != 0)
			continue;

		// find out what to do
		const char *pStatus = (const char *)(jsonFiles[i]["status"]);
		if(str_comp_nocase(pStatus, "modified") == 0 ||
		   str_comp_nocase(pStatus, "added") == 0)
		{
			// file has been added or modified since our version? -> queue for (re-)download
			m_DownloadJobs.push_back(std::string(pFilename));
		}
		else if(str_comp_nocase(pStatus, "renamed") == 0)
		{
			// if the file has only been renamed but not changed, we don't need to redownload it
			const char *pPreviousFilename = (const char *)(jsonFiles[i]["previous_filename"]);
			m_RenameJobs.push_back(std::pair<std::string, std::string>(std::string(pPreviousFilename), std::string()));
		}
		else if(str_comp_nocase(pStatus, "removed") == 0)
		{
			// files that have been removed from the repo can be deleted locally, too
			m_RemoveJobs.push_back(std::string(pFilename));
		}
	}

	json_value_free(&jsonCompare);

	return true;
}

//---------------------------- HELPER FUNCTIONS ----------------------------//

const std::string CGitHubAPI::SimpleGET(const char *pUrl)
{
	std::string Result = "";

	char aErr[CURL_ERROR_SIZE];
	mem_zerob(aErr);
	curl_easy_setopt(m_pHandle, CURLOPT_ERRORBUFFER, aErr);

	// create the headers
	curl_slist *list = NULL;
	list = curl_slist_append(list, "Content-Type: application/json");
	list = curl_slist_append(list, "User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:51.0) Gecko/20100101 Firefox/51.0");
	list = curl_slist_append(list, "Accept: application/vnd.github.v3+json");
	curl_easy_setopt(m_pHandle, CURLOPT_HTTPHEADER, list);

	curl_easy_setopt(m_pHandle, CURLOPT_CONNECTTIMEOUT_MS, (long)g_Config.m_ClHTTPConnectTimeoutMs);
	curl_easy_setopt(m_pHandle, CURLOPT_LOW_SPEED_LIMIT, (long)g_Config.m_ClHTTPLowSpeedLimit);
	curl_easy_setopt(m_pHandle, CURLOPT_LOW_SPEED_TIME, (long)g_Config.m_ClHTTPLowSpeedTime);
	curl_easy_setopt(m_pHandle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(m_pHandle, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(m_pHandle, CURLOPT_URL, pUrl);
	curl_easy_setopt(m_pHandle, CURLOPT_WRITEDATA, &Result);
	curl_easy_setopt(m_pHandle, CURLOPT_WRITEFUNCTION, &CCurlWrapper::CurlCallback_WriteToStdString);
	curl_easy_setopt(m_pHandle, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(m_pHandle, CURLOPT_NOSIGNAL, 1L);

	int ret = curl_easy_perform(m_pHandle);

	// clean up
	curl_slist_free_all(list);

	if(ret != CURLE_OK)
	{
		dbg_msg("github/error", "'%s' failed: %s", pUrl, aErr);
	}
	else
	{
		dbg_msg("github/debug", "'%s' -> %lu bytes", pUrl, Result.length());
	}
	return Result;
}

/* this one is overly complicated I think... let's not use it?
void CGitHubAPI::CurlWriteFunction(char *pData, size_t size, size_t nmemb, void *userdata)
{
	std::string *pResult = (std::string *)userdata;

	unsigned int BufferSize = (unsigned int)(size*nmemb + 1);
	char *pBuf = mem_allocb(char, BufferSize);
	mem_zero(pBuf, BufferSize);
	mem_copy(pBuf, pData, (unsigned int)(size*nmemb));

	*pResult += std::string(pBuf);
	mem_free(pBuf);
}*/

void CGitHubAPI::GitHashStr(const char *pFile, char *pBuffer, unsigned BufferSize)
{
	unsigned char aHash[SHA_DIGEST_LENGTH];
	mem_zerob(aHash);

	SHA_CTX context;
	if(!SHA_Init(&context))
	{
		dbg_msg("GitHashStr", "SHA_Init failed for '%s'", pFile);
	}
	else
	{
		IOHANDLE_SMART File(pFile, IOFLAG_READ);

		// prepend what git does
		{
			char aBuffer[16 * 1024];
			str_formatb(aBuffer, "blob %lu", File.Length());
			if(!SHA_Update(&context, aBuffer, (unsigned)str_length(aBuffer)+1)) // +1 because git wants the \0 to be hashed aswell!
			{
				dbg_msg("GitHashStr", "SHA_Update failed for the git identifier '%s'", aBuffer);
			}
			else
			{
				// read the data in portions of 16kb and hash it subsequently
				while(1)
				{
					mem_zerob(aBuffer);

					unsigned int BytesRead = File.Read(aBuffer, sizeof(aBuffer));
					if(BytesRead == 0)
						break;

					if(!SHA_Update(&context, aBuffer, BytesRead))
					{
						dbg_msg("GitHashStr", "SHA_Update failed for '%s' at 0x%x", pFile, (unsigned int)File.Tell());
						continue;
					}
				}
			}
		}
	}

	if(!SHA_Final(aHash, &context))
	{
		dbg_msg("GitHashStr", "SHA_Final failed for '%s'", pFile);
		mem_zerob(aHash);
	}

	str_hex_simple(pBuffer, BufferSize, aHash, SHA_DIGEST_LENGTH);
}