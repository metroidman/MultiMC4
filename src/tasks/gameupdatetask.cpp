// 
//  Copyright 2012 MultiMC Contributors
// 
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
// 
//        http://www.apache.org/licenses/LICENSE-2.0
// 
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//

#include "gameupdatetask.h"


#include <wx/wfstream.h>
#include <wx/sstream.h>
#include <wx/zipstrm.h>

#include <boost/property_tree/ini_parser.hpp>
// #include <boost/regex.hpp>

#include <md5/md5.h>

#include "utils/curlutils.h"
#include "utils/apputils.h"
#include <mcversionlist.h>
#include "downgradetask.h"


DEFINE_EVENT_TYPE(wxEVT_GAME_UPDATE_COMPLETE)

GameUpdateTask::GameUpdateTask(Instance *inst, int64_t latestVersion, bool forceUpdate)
	: Task(), m_inst(inst), m_latestVersion(latestVersion), m_forceUpdate(forceUpdate) {}

GameUpdateTask::~GameUpdateTask() {}

wxThread::ExitCode GameUpdateTask::TaskStart()
{
	wxString intendedVersion = m_inst->GetIntendedVersion();
	wxString currentVersion = m_inst->GetJarVersion();
	
	
	if(!m_inst->GetShouldUpdate() && !m_forceUpdate)
		return (ExitCode)1;
	
	SetState(STATE_DETERMINING_PACKAGES);
	
	MCVersionList & vlist = MCVersionList::Instance();
	vlist.LoadIfNeeded();
	
	MCVersion * ver = vlist.GetVersion(intendedVersion);
	
	if(!ver)
	{
		vlist.SetNeedsNostalgia();
		vlist.LoadIfNeeded();
		ver = vlist.GetVersion(intendedVersion);
		if(!ver)
			return (ExitCode)0;
	}
	
	bool do_patching = false;
	
	MCVersion * real_ver;
	wxString patch_version;
	if(ver->GetVersionType() == MCNostalgia)
	{
		do_patching = true;
		patch_version = ver->GetDescriptor();
		real_ver = vlist.GetCurrentStable();
	}
	else
	{
		real_ver = ver;
	}
	
	wxString mojangURL ("http://s3.amazonaws.com/MinecraftDownload/");
	jarURLs.clear();
	jarURLs.push_back(real_ver->GetDLUrl() + "minecraft.jar");
	jarURLs.push_back(mojangURL + "lwjgl_util.jar");
	jarURLs.push_back(mojangURL + "jinput.jar");
	jarURLs.push_back(mojangURL + "lwjgl.jar");

	wxString nativeJar;
#if WINDOWS
		nativeJar = "windows_natives.jar";
#elif OSX
		nativeJar = "macosx_natives.jar";
#elif LINUX
		nativeJar = "linux_natives.jar";
#else
#error Detected unsupported OS.
#endif
	jarURLs.push_back(mojangURL + nativeJar);
	
	SetProgress(5);
	
	wxFileName binDir = m_inst->GetBinDir();
	if (!binDir.DirExists())
		binDir.Mkdir();

	if(real_ver->GetVersionType() == CurrentStable)
		m_inst->WriteVersionFile(m_latestVersion);
	else
		m_inst->WriteVersionFile(real_ver->GetTimestamp() * 1000);
	DownloadJars();
	m_inst->UpdateVersion(false);
	m_inst->SetShouldUpdate(false);
	ExtractNatives();
	wxRemoveFile(Path::Combine(m_inst->GetBinDir(), wxFileName(jarURLs[jarURLs.size() - 1]).GetFullName()));
	m_inst->SetNeedsRebuild(true);
	if(do_patching)
	{
		DowngradeTask dtask(m_inst, patch_version);
		return dtask.Chain(this);
	}
	return (ExitCode)1;
}

bool GameUpdateTask::LoadJarURLs()
{

	return true;
}

void GameUpdateTask::AskToUpdate()
{
	// TODO Ask to update.
	
	// For now, we'll just assume the user doesn't want to update.
	m_shouldUpdate = false;
}

void GameUpdateTask::DownloadJars()
{
	using namespace boost::property_tree;
	ptree md5s;
	
	wxFileName md5File(m_inst->GetBinDir().GetFullPath(), "md5sums");
	if (md5File.FileExists())
	{
		try
		{
			read_ini(stdStr(md5File.GetFullPath()), md5s);
		}
		catch (ini_parser_error e)
		{
			wxLogError(_("Failed to parse md5s file.\nINI parser error at line %i: %s"), 
				e.line(), wxStr(e.message()).c_str());
		}
	}
	
	SetState(STATE_DOWNLOADING);
	
	int totalDownloadSize = 0;
	int *fileSizes = new int[jarURLs.size()];
	bool *skip = new bool[jarURLs.size()];
	
	// Compare MD5s and skip ones that match.
	for (size_t i = 0; i < jarURLs.size(); i++)
	{
		wxString etagOnDisk = wxStr(md5s.get<std::string>(
			stdStr(wxURL(jarURLs[i]).GetPath()), ""));
		
		struct curl_slist *headers = NULL;
		headers = curl_slist_append(headers, stdStr("If-None-Match: " + etagOnDisk).c_str());
		
		CURL *curl = InitCurlHandle();
		curl_easy_setopt(curl, CURLOPT_HEADER, true);
		curl_easy_setopt(curl, CURLOPT_URL, TOASCII(jarURLs[i]));
		curl_easy_setopt(curl, CURLOPT_NOBODY, true);
		
#ifdef HTTPDEBUG
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
#else
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlBlankCallback);
#endif
		
		if (curl_easy_perform(curl) != 0)
		{
			
		}
		
		long response = 0;
		double contentLen = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &contentLen);
		
		if (response == 300 && !m_forceUpdate)
			skip[i] = true;
		else
			skip[i] = false;
		
		fileSizes[i] = contentLen;
		totalDownloadSize += contentLen;
		
		curl_easy_cleanup(curl);
	}
	
	int initialProgress = 10;
	SetProgress(initialProgress);
	
	// Download jars
	int totalDownloadedSize = 0;
	for (size_t i = 0; i < jarURLs.size(); i++)
	{
		// Skip this file because we already have it.
		if (skip[i])
		{
			SetProgress(initialProgress + fileSizes[i] * 
				initialProgress / totalDownloadSize);
		}
		else
		{
			wxURL currentFile = jarURLs[i];

			SetState(STATE_DOWNLOADING, wxFileName(currentFile.GetPath()).GetFullName());

			wxFileName dlDest(m_inst->GetBinDir().GetFullPath(), 
							  wxFileName(currentFile.GetPath()).GetFullName());
			
			if (currentFile.GetURL().Contains("minecraft.jar") && 
				m_inst->GetMCBackup().FileExists())
			{
				wxRemoveFile(m_inst->GetMCBackup().GetFullPath());
			}
			
			const int maxDownloadTries = 5;
			int downloadTries = 0;
		DownloadFile:
			if (downloadTries >= maxDownloadTries)
			{
				EmitErrorMessage(_("Failed to download ") + currentFile.GetURL());
				return;
			}
			
			downloadTries++;
			
			unsigned char md5digest[16];
			int currentDownloadedSize = 0;
			
			CURL *curl = InitCurlHandle();
			curl_easy_setopt(curl, CURLOPT_URL, TOASCII(currentFile.GetURL()));
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlLambdaCallback);
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlLambdaCallback);
			
			wxFileOutputStream outStream(dlDest.GetFullPath());
			MD5Context md5ctx;
			MD5Init(&md5ctx);
			
			bool cancel = false;
			CurlLambdaCallbackFunction curlWrite = [&] (void *buffer, size_t size) -> size_t
			{
				currentDownloadedSize += size;
				totalDownloadedSize += size;
				
				outStream.Write(buffer, size);
				MD5Update(&md5ctx, (unsigned char*)buffer, size);
				
				SetProgress(initialProgress + 
					((double)totalDownloadedSize / (double)totalDownloadSize) * 
					(100 - initialProgress - 10));
				
				return outStream.LastWrite();
			};
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &curlWrite);
			
			wxString headerContent;
			wxStringOutputStream headerStream(&headerContent);
			CurlLambdaCallbackFunction curlWriteHeaders = [&] (void *buffer, size_t size) -> size_t
			{
				headerStream.Write(buffer, size);
				return headerStream.LastWrite();
			};
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &curlWriteHeaders);
			
			int errorCode = curl_easy_perform(curl);
			curl_easy_cleanup(curl);
			
			MD5Final(md5digest, &md5ctx);
			
// 			printf("MD5 for file %s: %s\n", 
// 				   stdStr(currentFile.GetURL()).c_str(),
// 				   stdStr(Utils::BytesToString(md5digest)).c_str());
			
			// Find the "ETag" in the headers
			const wxString etagHeader = "ETag: \"";
			
			size_t etagStart = headerContent.find(etagHeader) + etagHeader.Len();
			size_t etagEnd = headerContent.find("\"", etagStart) - 1;
			wxString etag = headerContent.SubString(etagStart, etagEnd);
			
			wxString md5sum = Utils::BytesToString(md5digest);
			
// 			printf("ETag:\t%s\n", cStr(etag));
// 			printf("MD5:\t%s\n", cStr(Utils::BytesToString(md5digest)));
			
			bool md5matches = md5sum.IsSameAs(etag, false);
			
			if (md5matches)
			{
				wxString keystr = wxFileName(currentFile.GetPath()).GetName();
				std::string key(TOASCII(keystr));
				// ASCII is fine. it's lower case letters and numbers
				std::string value (TOASCII(md5sum));
				md5s.put<std::string>(key, value);
				std::ofstream out;
				out.open(md5File.GetFullPath().mb_str());
				write_ini(out, md5s);
				out.flush();
				out.close();
			}
			else
			{
				totalDownloadedSize -= currentDownloadedSize;
				goto DownloadFile;
			}
		}
	}
}

void GameUpdateTask::ExtractNatives()
{
	SetState(STATE_EXTRACTING_PACKAGES);
	SetProgress(90);
	
	wxFileName nativesJar(Path::Combine(m_inst->GetBinDir(), 
		wxFileName(jarURLs[jarURLs.size() - 1]).GetFullName()));
	wxFileName nativesDir = wxFileName::DirName(Path::Combine(m_inst->GetBinDir(), 
															  "natives"));
	
	if (!nativesDir.DirExists())
		nativesDir.Mkdir();
	
	wxFileInputStream jarFileStream(nativesJar.GetFullPath());
	wxZipInputStream zipStream(jarFileStream);
	
	std::auto_ptr<wxZipEntry> entry;
	while (entry.reset(zipStream.GetNextEntry()), entry.get() != NULL)
	{
		if (entry->IsDir() || entry->GetInternalName().Contains("META-INF"))
			continue;
		SetState(STATE_EXTRACTING_PACKAGES, entry->GetName());
		wxFileName destFile(Path::Combine(nativesDir, entry->GetName()));
		wxFileOutputStream outStream(destFile.GetFullPath());
		outStream.Write(zipStream);
	}
}


void GameUpdateTask::SetState(UpdateState state, const wxString& msg)
{
	switch (state)
	{
	case STATE_INIT:
		if (msg == wxEmptyString)
			SetStatus(_("Initializing..."));
		else
			SetStatus(_("Initializing: ") + msg);
		break;
		
	case STATE_DETERMINING_PACKAGES:
		if (msg == wxEmptyString)
			SetStatus(_("Determining packages to load..."));
		else
			SetStatus(_("Determining packages to load: ") + msg);
		break;
		
	case STATE_CHECKING_CACHE:
		if (msg == wxEmptyString)
			SetStatus(_("Checking cache for existing files..."));
		else
			SetStatus(_("Checking cache for existing files: ") + msg);
		break;
		
	case STATE_DOWNLOADING:
		if (msg == wxEmptyString)
			SetStatus(_("Downloading packages..."));
		else
			SetStatus(_("Downloading packages: ") + msg);
		break;
		
	case STATE_EXTRACTING_PACKAGES:
		if (msg == wxEmptyString)
			SetStatus(_("Extracting downloaded packages..."));
		else
			SetStatus(_("Extracting downloaded packages: ") + msg);
		break;
		
	default:
		break;
	}
}
