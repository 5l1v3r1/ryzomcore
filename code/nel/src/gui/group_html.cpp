// Ryzom - MMORPG Framework <http://dev.ryzom.com/projects/ryzom/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

//#include <crtdbg.h>

#include "stdpch.h"
#include "nel/gui/group_html.h"

#include <string>
#include "nel/misc/types_nl.h"
#include "nel/misc/rgba.h"
#include "nel/misc/algo.h"
#include "nel/gui/libwww.h"
#include "nel/gui/group_html.h"
#include "nel/gui/group_list.h"
#include "nel/gui/group_menu.h"
#include "nel/gui/group_container.h"
#include "nel/gui/view_link.h"
#include "nel/gui/ctrl_scroll.h"
#include "nel/gui/ctrl_button.h"
#include "nel/gui/ctrl_text_button.h"
#include "nel/gui/action_handler.h"
#include "nel/gui/group_paragraph.h"
#include "nel/gui/group_editbox.h"
#include "nel/gui/widget_manager.h"
#include "nel/gui/lua_manager.h"
#include "nel/gui/view_bitmap.h"
#include "nel/gui/dbgroup_combo_box.h"
#include "nel/gui/lua_ihm.h"
#include "nel/misc/i18n.h"
#include "nel/misc/md5.h"
#include "nel/3d/texture_file.h"
#include "nel/misc/big_file.h"
#include "nel/gui/url_parser.h"
#include "nel/gui/http_cache.h"
#include "nel/gui/http_hsts.h"
#include "nel/gui/curl_certificates.h"
#include "nel/gui/html_parser.h"
#include "nel/gui/html_element.h"
#include "nel/gui/css_style.h"
#include "nel/gui/css_parser.h"

#include <curl/curl.h>

using namespace std;
using namespace NLMISC;

#ifdef DEBUG_NEW
#define new DEBUG_NEW
#endif

// Default maximum time the request is allowed to take
#define DEFAULT_RYZOM_CONNECTION_TIMEOUT (300.0)
// Allow up to 10 redirects, then give up
#define DEFAULT_RYZOM_REDIRECT_LIMIT (10)
//
#define FONT_WEIGHT_NORMAL 400
#define FONT_WEIGHT_BOLD 700

namespace NLGUI
{
	// Uncomment to see the log about image download
	//#define LOG_DL 1

	CGroupHTML::SWebOptions CGroupHTML::options;

	// Return URL with https is host is in HSTS list
	static std::string upgradeInsecureUrl(const std::string &url)
	{
		if (toLower(url.substr(0, 7)) != "http://") {
			return url;
		}

		CUrlParser uri(url);
		if (!CStrictTransportSecurity::getInstance()->isSecureHost(uri.host)){
			return url;
		}

	#ifdef LOG_DL
		nlwarning("HSTS url : '%s', using https", url.c_str());
	#endif
		uri.scheme = "https";

		return uri.toString();
	}

	// Active cURL www transfer
	class CCurlWWWData
	{
		public:
			CCurlWWWData(CURL *curl, const std::string &url)
				: Request(curl), Url(url), Content(""), HeadersSent(NULL)
			{
			}
			~CCurlWWWData()
			{
				if (Request)
					curl_easy_cleanup(Request);

				if (HeadersSent)
					curl_slist_free_all(HeadersSent);
			}

			void sendHeaders(const std::vector<std::string> headers)
			{
				for(uint i = 0; i < headers.size(); ++i)
				{
					HeadersSent = curl_slist_append(HeadersSent, headers[i].c_str());
				}
				curl_easy_setopt(Request, CURLOPT_HTTPHEADER, HeadersSent);
			}

			void setRecvHeader(const std::string &header)
			{
				size_t pos = header.find(": ");
				if (pos == std::string::npos)
					return;

				std::string key = toLower(header.substr(0, pos));
				if (pos != std::string::npos)
				{
					HeadersRecv[key] = header.substr(pos + 2);
					//nlinfo(">> received header '%s' = '%s'", key.c_str(), HeadersRecv[key].c_str());
				}
			}

			// return last received "Location: <url>" header or empty string if no header set
			const std::string getLocationHeader()
			{
				if (HeadersRecv.count("location") > 0)
					return HeadersRecv["location"];

				return "";
			}

			const uint32 getExpires()
			{
				time_t ret = 0;
				if (HeadersRecv.count("expires") > 0)
					ret = curl_getdate(HeadersRecv["expires"].c_str(), NULL);

				return ret > -1 ? ret : 0;
			}

			const std::string getLastModified()
			{
				if (HeadersRecv.count("last-modified") > 0)
				{
					return HeadersRecv["last-modified"];
				}

				return "";
			}

			const std::string getEtag()
			{
				if (HeadersRecv.count("etag") > 0)
				{
					return HeadersRecv["etag"];
				}

				return "";
			}

			bool hasHSTSHeader()
			{
				// ignore header if not secure connection
				if (toLower(Url.substr(0, 8)) != "https://")
				{
					return false;
				}

				return HeadersRecv.count("strict-transport-security") > 0;
			}

			const std::string getHSTSHeader()
			{
				if (hasHSTSHeader())
				{
					return HeadersRecv["strict-transport-security"];
				}

				return "";
			}

		public:
			CURL *Request;

			std::string Url;
			std::string Content;

		private:
			// headers sent with curl request, must be released after transfer
			curl_slist * HeadersSent;

			// headers received from curl transfer
			std::map<std::string, std::string> HeadersRecv;
	};

	// cURL transfer callbacks
	// ***************************************************************************
	static size_t curlHeaderCallback(char *buffer, size_t size, size_t nmemb, void *pCCurlWWWData)
	{
		CCurlWWWData * me = static_cast<CCurlWWWData *>(pCCurlWWWData);
		if (me)
		{
			std::string header;
			header.append(buffer, size * nmemb);
			me->setRecvHeader(header.substr(0, header.find_first_of("\n\r")));
		}

		return size * nmemb;
	}

	// ***************************************************************************
	static size_t curlDataCallback(char *buffer, size_t size, size_t nmemb, void *pCCurlWWWData)
	{
		CCurlWWWData * me = static_cast<CCurlWWWData *>(pCCurlWWWData);
		if (me)
			me->Content.append(buffer, size * nmemb);

		return size * nmemb;
	}

	// ***************************************************************************
	static size_t curlProgressCallback(void *pCCurlWWWData, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
	{
		CCurlWWWData * me = static_cast<CCurlWWWData *>(pCCurlWWWData);
		if (me)
		{
			if (dltotal > 0 || dlnow > 0 || ultotal > 0 || ulnow > 0)
			{
				nlwarning("> dltotal %d, dlnow %d, ultotal %d, ulnow %d, url '%s'", dltotal, dlnow, ultotal, ulnow, me->Url.c_str());
			}
		}

		// return 1 to cancel download
		return 0;
	}

	// Check if domain is on TrustedDomain
	bool CGroupHTML::isTrustedDomain(const string &domain)
	{
		vector<string>::iterator it;
		it = find ( options.trustedDomains.begin(), options.trustedDomains.end(), domain);
		return it != options.trustedDomains.end();
	}

	// Update view after download has finished
	void CGroupHTML::setImage(CViewBase * view, const string &file, const TImageType type)
	{
		CCtrlButton *btn = dynamic_cast<CCtrlButton*>(view);
		if(btn)
		{
			if (type == NormalImage)
			{
				btn->setTexture (file);
				btn->setTexturePushed(file);
				btn->invalidateCoords();
				btn->invalidateContent();
				paragraphChange();
			}
			else
			{
				btn->setTextureOver(file);
			}
		}
		else
		{
			CViewBitmap *btm = dynamic_cast<CViewBitmap*>(view);
			if(btm)
			{
				btm->setTexture (file);
				btm->invalidateCoords();
				btm->invalidateContent();
				paragraphChange();
			}
			else
			{
				CGroupCell *btgc = dynamic_cast<CGroupCell*>(view);
				if(btgc)
				{
					btgc->setTexture (file);
					btgc->invalidateCoords();
					btgc->invalidateContent();
					paragraphChange();
				}
			}
		}
	}

	// Force image width, height
	void CGroupHTML::setImageSize(CViewBase *view, const CStyleParams &style)
	{
		sint32 width = style.Width;
		sint32 height = style.Height;
		sint32 maxw = style.MaxWidth;
		sint32 maxh = style.MaxHeight;
		
		sint32 imageWidth, imageHeight;
		bool changed = true;
		
		// get image texture size
		// if image is being downloaded, then correct size is set after thats done
		CCtrlButton *btn = dynamic_cast<CCtrlButton*>(view);
		if(btn)
		{
			btn->fitTexture();
			imageWidth = btn->getW(false);
			imageHeight = btn->getH(false);
		}
		else
		{
			CViewBitmap *btm = dynamic_cast<CViewBitmap*>(view);
			if(btm)
			{
				btm->fitTexture();
				imageWidth = btm->getW(false);
				imageHeight = btm->getH(false);
			}
			else
			{
				// not supported
				return;
			}
		}
		
		// if width/height is not requested, then use image size
		// else recalculate missing value, keep image ratio
		if (width == -1 && height == -1)
		{
			width = imageWidth;
			height = imageHeight;
			
			changed = false;
		}
		else
		if (width == -1 || height == -1) {
			float ratio = (float) imageWidth / std::max(1, imageHeight);
			if (width == -1)
				width = height * ratio;
			else
				height = width / ratio;
		}
		
		// apply max-width, max-height rules if asked
		if (maxw > -1 || maxh > -1)
		{
			_Style.applyCssMinMax(width, height, 0, 0, maxw, maxh);
			changed = true;
		}

		if (changed)
		{
			CCtrlButton *btn = dynamic_cast<CCtrlButton*>(view);
			if(btn)
			{
				btn->setScale(true);
				btn->setW(width);
				btn->setH(height);
			}
			else
			{
				CViewBitmap *image = dynamic_cast<CViewBitmap*>(view);
				if(image)
				{
					image->setScale(true);
					image->setW(width);
					image->setH(height);
				}
			}
		}
	}

	void CGroupHTML::setTextButtonStyle(CCtrlTextButton *ctrlButton, const CStyleParams &style)
	{
		// this will also set size for <a class="ryzom-ui-button"> treating it like "display: inline-block;"
		if (style.Width > 0)  ctrlButton->setWMin(style.Width);
		if (style.Height > 0) ctrlButton->setHMin(style.Height);

		CViewText *pVT = ctrlButton->getViewText();
		if (pVT)
		{
			setTextStyle(pVT, style);
		}

		if (style.hasStyle("background-color"))
		{
			ctrlButton->setColor(style.BackgroundColor);
			if (style.hasStyle("-ryzom-background-color-over"))
			{
				ctrlButton->setColorOver(style.BackgroundColorOver);
			}
			else
			{
				ctrlButton->setColorOver(style.BackgroundColor);
			}
			ctrlButton->setTexture("", "blank.tga", "", false);
			ctrlButton->setTextureOver("", "blank.tga", "");
			ctrlButton->setProperty("force_text_over", "true");
		}
		else if (style.hasStyle("-ryzom-background-color-over"))
		{
			ctrlButton->setColorOver(style.BackgroundColorOver);
			ctrlButton->setProperty("force_text_over", "true");
			ctrlButton->setTextureOver("blank.tga", "blank.tga", "blank.tga");
		}
	}

	void CGroupHTML::setTextStyle(CViewText *pVT, const CStyleParams &style)
	{
		if (pVT)
		{
			pVT->setFontSize(style.FontSize);
			pVT->setColor(style.TextColor);
			pVT->setFontName(style.FontFamily);
			pVT->setFontSize(style.FontSize);
			pVT->setEmbolden(style.FontWeight >= FONT_WEIGHT_BOLD);
			pVT->setOblique(style.FontOblique);
			pVT->setUnderlined(style.Underlined);
			pVT->setStrikeThrough(style.StrikeThrough);
			if (style.TextShadow.Enabled)
			{
				pVT->setShadow(true);
				pVT->setShadowColor(style.TextShadow.Color);
				pVT->setShadowOutline(style.TextShadow.Outline);
				pVT->setShadowOffset(style.TextShadow.X, style.TextShadow.Y);
			}
		}
	}

	// Get an url and return the local filename with the path where the url image should be
	string CGroupHTML::localImageName(const string &url)
	{
		string dest = "cache/";
		dest += getMD5((uint8 *)url.c_str(), (uint32)url.size()).toString();
		dest += ".cache";
		return dest;
	}

	void CGroupHTML::pumpCurlDownloads()
	{
		if (RunningCurls < options.curlMaxConnections)
		{
			for (vector<CDataDownload>::iterator it=Curls.begin(); it<Curls.end(); it++)
			{
				if (it->data == NULL)
				{
					#ifdef LOG_DL
					nlwarning("(%s) starting new download '%s'", _Id.c_str(), it->url.c_str());
					#endif
					if (!startCurlDownload(*it))
					{
						finishCurlDownload(*it);
						Curls.erase(it);
						break;
					}

					RunningCurls++;
					if (RunningCurls >= options.curlMaxConnections)
						break;
				}
			}
		}
		#ifdef LOG_DL
		if (RunningCurls > 0 || !Curls.empty())
			nlwarning("(%s) RunningCurls %d, _Curls %d", _Id.c_str(), RunningCurls, Curls.size());
		#endif
	}

	// Add url to MultiCurl queue and return cURL handle
	bool CGroupHTML::startCurlDownload(CDataDownload &download)
	{
		if (!MultiCurl)
		{
			nlwarning("Invalid MultiCurl handle, unable to download '%s'", download.url.c_str());
			return false;
		}

		time_t currentTime;
		time(&currentTime);

		CHttpCacheObject cache;
		if (CFile::fileExists(download.dest))
			cache = CHttpCache::getInstance()->lookup(download.dest);

		if (cache.Expires > currentTime)
		{
	#ifdef LOG_DL
			nlwarning("Cache for (%s) is not expired (%s, expires:%d)", download.url.c_str(), download.dest.c_str(), cache.Expires - currentTime);
	#endif
			return false;
		}

		string tmpdest = download.dest + ".tmp";

		// erase the tmp file if exists
		if (CFile::fileExists(tmpdest))
			CFile::deleteFile(tmpdest);

		FILE *fp = nlfopen (tmpdest, "wb");
		if (fp == NULL)
		{
			nlwarning("Can't open file '%s' for writing: code=%d '%s'", tmpdest.c_str (), errno, strerror(errno));
			return false;
		}

		CURL *curl = curl_easy_init();
		if (!curl)
		{
			fclose(fp);
			CFile::deleteFile(tmpdest);

			nlwarning("Creating cURL handle failed, unable to download '%s'", download.url.c_str());
			return false;
		}

		// https://
		if (toLower(download.url.substr(0, 8)) == "https://")
		{
			// if supported, use custom SSL context function to load certificates
			CCurlCertificates::useCertificates(curl);
		}

		download.data = new CCurlWWWData(curl, download.url);
		download.fp = fp;

		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, true);
		curl_easy_setopt(curl, CURLOPT_URL, download.url.c_str());

		// limit curl to HTTP and HTTPS protocols only
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);

		std::vector<std::string> headers;
		if (!cache.Etag.empty())
			headers.push_back("If-None-Match: " + cache.Etag);

		if (!cache.LastModified.empty())
			headers.push_back("If-Modified-Since: " + cache.LastModified);

		if (headers.size() > 0)
			download.data->sendHeaders(headers);

		// catch headers
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NLGUI::curlHeaderCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEHEADER, download.data);

		std::string userAgent = options.appName + "/" + options.appVersion;
		curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());

		CUrlParser uri(download.url);
		if (!uri.host.empty())
			sendCookies(curl, uri.host, isTrustedDomain(uri.host));

		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);

		curl_multi_add_handle(MultiCurl, curl);

		return true;
	}

	void CGroupHTML::finishCurlDownload(CDataDownload &download)
	{
		std::string tmpfile = download.dest + ".tmp";

		if (download.type == ImgType)
		{
			// there is race condition if two browser instances are downloading same file
			// second instance deletes first tmpfile and creates new file for itself.
			if (CFile::getFileSize(tmpfile) > 0)
			{
				try
				{
					// verify that image is not corrupted
					uint32 w, h;
					CBitmap::loadSize(tmpfile, w, h);
					if (w != 0 && h != 0)
					{
						bool refresh = false;
						if (CFile::fileExists(download.dest))
						{
							CFile::deleteFile(download.dest);
							refresh = true;
						}

						// to reload image on page, the easiest seems to be changing texture
						// to temp file temporarily. that forces driver to reload texture from disk
						// ITexture::touch() seem not to do this.
						if (refresh)
						{
							// cache was updated, first set texture as temp file
							for(uint i = 0; i < download.imgs.size(); i++)
							{
								setImage(download.imgs[i].Image, tmpfile, download.imgs[i].Type);
								setImageSize(download.imgs[i].Image, download.imgs[i].Style);
							}
						}

						// move temp to correct cache file
						CFile::moveFile(download.dest, tmpfile);
						// set image texture as cache file
						for(uint i = 0; i < download.imgs.size(); i++)
						{
							setImage(download.imgs[i].Image, download.dest, download.imgs[i].Type);
							setImageSize(download.imgs[i].Image, download.imgs[i].Style);
						}

					}
				}
				catch(const NLMISC::Exception &e)
				{
					// exception message has .tmp file name, so keep it for further analysis
					nlwarning("Invalid image (%s): %s", download.url.c_str(), e.what());
				}
			}

			return;
		}

		if (!tmpfile.empty())
		{
			CFile::moveFile(download.dest, tmpfile);
		}

		if (download.type == StylesheetType)
		{
			cssDownloadFinished(download.url, download.dest);

			return;
		}

		if (download.type == BnpType)
		{
			CLuaManager::getInstance().executeLuaScript(download.luaScript, true );

			return;
		}

		nlwarning("Unknown CURL download type (%d) finished '%s'", download.type, download.url.c_str());
	}

	// Add a image download request in the multi_curl
	void CGroupHTML::addImageDownload(const string &url, CViewBase *img, const CStyleParams &style, TImageType type)
	{
		string finalUrl = upgradeInsecureUrl(getAbsoluteUrl(url));

		// use requested url for local name (cache)
		string dest = localImageName(url);
	#ifdef LOG_DL
		nlwarning("add to download '%s' dest '%s' img %p", finalUrl.c_str(), dest.c_str(), img);
	#endif

		// Display cached image while downloading new
		if (type != OverImage && CFile::fileExists(dest))
		{
			setImage(img, dest, type);
			setImageSize(img, style);
		}

		// Search if we are not already downloading this url.
		for(uint i = 0; i < Curls.size(); i++)
		{
			if(Curls[i].url == finalUrl)
			{
	#ifdef LOG_DL
				nlwarning("already downloading '%s' img %p", finalUrl.c_str(), img);
	#endif
				Curls[i].imgs.push_back(CDataImageDownload(img, style, type));
				return;
			}
		}

		Curls.push_back(CDataDownload(finalUrl, dest, ImgType, img, "", "", style, type));

		pumpCurlDownloads();
	}

	void CGroupHTML::initImageDownload()
	{
	#ifdef LOG_DL
		nlwarning("Init Image Download");
	#endif

		string pathName = "cache";
		if ( ! CFile::isExists( pathName ) )
			CFile::createDirectory( pathName );
	}


	// Get an url and return the local filename with the path where the bnp should be
	string CGroupHTML::localBnpName(const string &url)
	{
		size_t lastIndex = url.find_last_of("/");
		string dest = "user/"+url.substr(lastIndex+1);
		return dest;
	}

	// Add a bnp download request in the multi_curl, return true if already downloaded
	bool CGroupHTML::addBnpDownload(string url, const string &action, const string &script, const string &md5sum)
	{
		url = upgradeInsecureUrl(getAbsoluteUrl(url));

		// Search if we are not already downloading this url.
		for(uint i = 0; i < Curls.size(); i++)
		{
			if(Curls[i].url == url)
			{
	#ifdef LOG_DL
				nlwarning("already downloading '%s'", url.c_str());
	#endif
				return false;
			}
		}

		string dest = localBnpName(url);
	#ifdef LOG_DL
		nlwarning("add to download '%s' dest '%s'", url.c_str(), dest.c_str());
	#endif

		// create/delete the local file
		if (NLMISC::CFile::fileExists(dest))
		{
			if (action == "override" || action == "delete")
			{
				CFile::setRWAccess(dest);
				NLMISC::CFile::deleteFile(dest);
			}
			else
			{
				return true;
			}
		}
		if (action != "delete")
		{
			Curls.push_back(CDataDownload(url, dest, BnpType, NULL, script, md5sum));

			pumpCurlDownloads();
		}
		else
			return true;

		return false;
	}

	void CGroupHTML::initBnpDownload()
	{
		if (!_TrustedDomain)
			return;

	#ifdef LOG_DL
		nlwarning("Init Bnp Download");
	#endif
		string pathName = "user";
		if ( ! CFile::isExists( pathName ) )
			CFile::createDirectory( pathName );
	}

	void CGroupHTML::addStylesheetDownload(std::vector<std::string> links)
	{
		for(uint i = 0; i < links.size(); ++i)
		{
			std::string url = getAbsoluteUrl(links[i]);
			std::string local = localImageName(url);

			// insert only if url not already downloading
			std::vector<std::string>::const_iterator it = std::find(_StylesheetQueue.begin(), _StylesheetQueue.end(), url);
			if (it == _StylesheetQueue.end())
			{
				_StylesheetQueue.push_back(url);
				Curls.push_back(CDataDownload(url, local, StylesheetType, NULL, "", ""));
			}
		}

		pumpCurlDownloads();
	}

	// Call this evenly to check if an element is downloaded and then manage it
	void CGroupHTML::checkDownloads()
	{
		//nlassert(_CrtCheckMemory());

		if(Curls.empty() && _CurlWWW == NULL)
			return;

		int NewRunningCurls = 0;
		while(CURLM_CALL_MULTI_PERFORM == curl_multi_perform(MultiCurl, &NewRunningCurls))
		{
	#ifdef LOG_DL
			nlwarning("more to do now %d - %d curls", NewRunningCurls, Curls.size());
	#endif
		}
		if(NewRunningCurls < RunningCurls)
		{
			// some download are done, callback them
	#ifdef LOG_DL
			nlwarning ("new %d old %d", NewRunningCurls, RunningCurls);
	#endif
			// check msg
			CURLMsg *msg;
			int msgs_left;
			while ((msg = curl_multi_info_read(MultiCurl, &msgs_left)))
			{
	#ifdef LOG_DL
				nlwarning("> (%s) msgs_left %d", _Id.c_str(), msgs_left);
	#endif
				if (msg->msg == CURLMSG_DONE)
				{
					if (_CurlWWW && _CurlWWW->Request && _CurlWWW->Request == msg->easy_handle)
					{
						CURLcode res = msg->data.result;
						long code;
						curl_easy_getinfo(_CurlWWW->Request, CURLINFO_RESPONSE_CODE, &code);
	#ifdef LOG_DL
						nlwarning("(%s) web transfer '%p' completed with status %d, http %d, url (len %d) '%s'", _Id.c_str(), _CurlWWW->Request, res, code, _CurlWWW->Url.size(), _CurlWWW->Url.c_str());
	#endif
						// save HSTS header from all requests regardless of HTTP code
						if (res == CURLE_OK && _CurlWWW->hasHSTSHeader())
						{
							CUrlParser uri(_CurlWWW->Url);
							CStrictTransportSecurity::getInstance()->setFromHeader(uri.host, _CurlWWW->getHSTSHeader());
						}

						if (res != CURLE_OK)
						{
							std::string err;
							err = "Connection failed with cURL error: ";
							err += curl_easy_strerror(res);
							err += "\nURL '" + _CurlWWW->Url + "'";
							browseError(err.c_str());
						}
						else
						if ((code >= 301 && code <= 303) || code == 307 || code == 308)
						{
							if (_RedirectsRemaining < 0)
							{
								browseError(string("Redirect limit reached : " + _URL).c_str());
							}
							else
							{
								receiveCookies(_CurlWWW->Request, _DocumentDomain, _TrustedDomain);

								// redirect, get the location and try browse again
								// we cant use curl redirection because 'addHTTPGetParams()' must be called on new destination
								std::string location(_CurlWWW->getLocationHeader());
								if (!location.empty())
								{
	#ifdef LOG_DL
									nlwarning("(%s) request (%d) redirected to (len %d) '%s'", _Id.c_str(), _RedirectsRemaining, location.size(), location.c_str());
	#endif
									location = getAbsoluteUrl(location);
									// throw away this handle and start with new one (easier than reusing)
									requestTerminated();

									_PostNextTime = false;
									_RedirectsRemaining--;

									doBrowse(location.c_str());
								}
								else
								{
									browseError(string("Request was redirected, but location was not set : "+_URL).c_str());
								}
							}
						}
						else
						{
							receiveCookies(_CurlWWW->Request, _DocumentDomain, _TrustedDomain);

							_RedirectsRemaining = DEFAULT_RYZOM_REDIRECT_LIMIT;

							if ( (code < 200 || code >= 300) )
							{
								browseError(string("Connection failed (curl code " + toString((sint32)res) + ")\nhttp code " + toString((sint32)code) + ")\nURL '" + _CurlWWW->Url + "'").c_str());
							}
							else
							{
								char *ch;
								std::string contentType;
								res = curl_easy_getinfo(_CurlWWW->Request, CURLINFO_CONTENT_TYPE, &ch);
								if (res == CURLE_OK && ch != NULL)
								{
									contentType = ch;
								}

								htmlDownloadFinished(_CurlWWW->Content, contentType, code);
							}
							requestTerminated();
						}

						continue;
					}

					for (vector<CDataDownload>::iterator it=Curls.begin(); it<Curls.end(); it++)
					{
						if(it->data && it->data->Request == msg->easy_handle)
						{
							CURLcode res = msg->data.result;
							long r;
							curl_easy_getinfo(it->data->Request, CURLINFO_RESPONSE_CODE, &r);
							fclose(it->fp);

							CUrlParser uri(it->url);
							if (!uri.host.empty())
								receiveCookies(it->data->Request, uri.host, isTrustedDomain(uri.host));
	#ifdef LOG_DL
							nlwarning("(%s) transfer '%p' completed with status %d, http %d, url (len %d) '%s'", _Id.c_str(), it->data->Request, res, r, it->url.size(), it->url.c_str());
	#endif
							curl_multi_remove_handle(MultiCurl, it->data->Request);

							// save HSTS header from all requests regardless of HTTP code
							if (res == CURLE_OK && it->data->hasHSTSHeader())
							{
								CStrictTransportSecurity::getInstance()->setFromHeader(uri.host, it->data->getHSTSHeader());
							}

							std::string tmpfile = it->dest + ".tmp";
							if(res != CURLE_OK || r < 200 || r >= 300 || (!it->md5sum.empty() && (it->md5sum != getMD5(tmpfile).toString())))
							{
								if (it->redirects < DEFAULT_RYZOM_REDIRECT_LIMIT && ((r >= 301 && r <= 303) || r == 307 || r == 308))
								{
									std::string location(it->data->getLocationHeader());
									if (!location.empty())
									{
										CUrlParser uri(location);
										if (!uri.isAbsolute())
										{
											uri.inherit(it->url);
											location = uri.toString();
										}

										it->url = location;
										it->fp = NULL;

										// release CCurlWWWData
										delete it->data;
										it->data = NULL;

										it->redirects++;
	#ifdef LOG_DL
										nlwarning("Redirect '%s'", location.c_str());
	#endif
										// keep the request in queue
										continue;
									}
									else
										nlwarning("Redirected to empty url '%s'", it->url.c_str());
								}
								else
								{
									if (it->redirects >= DEFAULT_RYZOM_REDIRECT_LIMIT)
										nlwarning("Redirect limit reached for '%s'", it->url.c_str());

									CFile::deleteFile(tmpfile);

									// 304 Not Modified
									if (res == CURLE_OK && r == 304)
									{
										CHttpCacheObject obj;
										obj.Expires = it->data->getExpires();
										obj.Etag = it->data->getEtag();
										obj.LastModified = it->data->getLastModified();

										CHttpCache::getInstance()->store(it->dest, obj);
									}
									else
									{
										// 404, 500, etc
										if (CFile::fileExists(it->dest))
											CFile::deleteFile(it->dest);
									}

									finishCurlDownload(*it);
								}
							}
							else
							{
								CHttpCacheObject obj;
								obj.Expires = it->data->getExpires();
								obj.Etag = it->data->getEtag();
								obj.LastModified = it->data->getLastModified();

								CHttpCache::getInstance()->store(it->dest, obj);

								finishCurlDownload(*it);
							}

							// release CCurlWWWData
							delete it->data;

							Curls.erase(it);
							break;
						}
					}
				}
			}
		}

		RunningCurls = NewRunningCurls;

		pumpCurlDownloads();
	}


	void CGroupHTML::releaseDownloads()
	{
	#ifdef LOG_DL
		nlwarning("Release Downloads");
	#endif

		// remove all queued and already started downloads
		for (uint i = 0; i < Curls.size(); ++i)
		{
			if (Curls[i].data)
			{
				if (MultiCurl)
					curl_multi_remove_handle(MultiCurl, Curls[i].data->Request);

				// release CCurlWWWData
				delete Curls[i].data;
			}
		}
		Curls.clear();
	}

	class CGroupListAdaptor : public CInterfaceGroup
	{
	public:
		CGroupListAdaptor(const TCtorParam &param)
			: CInterfaceGroup(param)
		{}

	private:
		void updateCoords()
		{
			if (_Parent)
			{
				// Get the W max from the parent
				_W = std::min(_Parent->getMaxWReal(), _Parent->getWReal());
				_WReal = _W;
			}
			CInterfaceGroup::updateCoords();
		}
	};

	// ***************************************************************************

	template<class A> void popIfNotEmpty(A &vect) { if(!vect.empty()) vect.pop_back(); }

	// ***************************************************************************

	void CGroupHTML::beginBuild ()
	{
		if (_Browsing)
		{
			_Connecting = false;

			removeContent ();
		}
		else
			nlwarning("_Browsing = FALSE");
	}


	TStyle CGroupHTML::parseStyle (const string &str_styles)
	{
		TStyle styles;
		vector<string> elements;
		NLMISC::splitString(str_styles, ";", elements);

		for(uint i = 0; i < elements.size(); ++i)
		{
			vector<string> style;
			NLMISC::splitString(elements[i], ":", style);
			if (style.size() >= 2)
			{
				string fullstyle = style[1];
				for (uint j=2; j < style.size(); j++)
					fullstyle += ":"+style[j];
				styles[trim(style[0])] = trim(fullstyle);
			}
		}

		return styles;
	}

	// ***************************************************************************

	void CGroupHTML::addText (const char * buf, int len)
	{
		if (_Browsing)
		{
			if (_IgnoreText)
				return;

			// Build a UTF8 string
			string inputString(buf, buf+len);

			if (_ParsingLua && _TrustedDomain)
			{
				// we are parsing a lua script
				_LuaScript += inputString;
				// no more to do
				return;
			}

			// Build a unicode string
			ucstring inputUCString;
			inputUCString.fromUtf8(inputString);

			// Build the final unicode string
			ucstring tmp;
			tmp.reserve(len);
			uint ucLen = (uint)inputUCString.size();
			for (uint i=0; i<ucLen; i++)
			{
				ucchar output;
				bool keep;
				// special treatment for 'nbsp' (which is returned as a discreet space)
				if (inputString.size() == 1 && inputString[0] == 32)
				{
					// this is a nbsp entity
					output = inputUCString[i];
					keep = true;
				}
				else
				{
					// not nbsp, use normal white space removal routine
					keep = translateChar (output, inputUCString[i], (tmp.empty())?0:tmp[tmp.size()-1]);
				}

				if (keep)
				{
					tmp.push_back(output);
				}
			}

			if (!tmp.empty())
				addString(tmp);
		}
	}

	// ***************************************************************************

	#define registerAnchorName(prefix) \
	{\
		if (present[prefix##_ID] && value[prefix##_ID]) \
			_AnchorName.push_back(value[prefix##_ID]); \
	}

	// ***************************************************************************

	#define getCellsParameters_DEP(prefix,inherit) \
	{\
		CGroupHTML::CCellParams cellParams; \
		if (!_CellParams.empty() && inherit) \
		{ \
			cellParams = _CellParams.back(); \
		} \
		if (present[prefix##_BGCOLOR] && value[prefix##_BGCOLOR]) \
			scanHTMLColor(value[prefix##_BGCOLOR], cellParams.BgColor); \
		if (present[prefix##_L_MARGIN] && value[prefix##_L_MARGIN]) \
			fromString(value[prefix##_L_MARGIN], cellParams.LeftMargin); \
		if (present[prefix##_NOWRAP]) \
			cellParams.NoWrap = true; \
		if (present[prefix##_ALIGN] && value[prefix##_ALIGN]) \
		{ \
			string align = toLower(value[prefix##_ALIGN]); \
			if (align == "left") \
				cellParams.Align = CGroupCell::Left; \
			if (align == "center") \
				cellParams.Align = CGroupCell::Center; \
			if (align == "right") \
				cellParams.Align = CGroupCell::Right; \
		} \
		if (present[prefix##_VALIGN] && value[prefix##_VALIGN]) \
		{ \
			string align = toLower(value[prefix##_VALIGN]); \
			if (align == "top") \
				cellParams.VAlign = CGroupCell::Top; \
			if (align == "middle") \
				cellParams.VAlign = CGroupCell::Middle; \
			if (align == "bottom") \
				cellParams.VAlign = CGroupCell::Bottom; \
		} \
		_CellParams.push_back (cellParams); \
	}

	// ***************************************************************************
	void CGroupHTML::beginElement (CHtmlElement &elm)
	{
		_Style.pushStyle();
		_CurrentHTMLElement = &elm;
		_CurrentHTMLNextSibling = elm.nextSibling;

		// set element style from css and style attribute
		_Style.getStyleFor(elm);
		if (!elm.Style.empty())
		{
			_Style.applyStyle(elm.Style);
		}

		if (elm.hasNonEmptyAttribute("name"))
		{
			_AnchorName.push_back(elm.getAttribute("name"));
		}
		if (elm.hasNonEmptyAttribute("id"))
		{
			_AnchorName.push_back(elm.getAttribute("id"));
		}

		switch(elm.ID)
		{
		case HTML_A:        htmlA(elm); break;
		case HTML_BASE:     htmlBASE(elm); break;
		case HTML_BODY:     htmlBODY(elm); break;
		case HTML_BR:       htmlBR(elm); break;
		case HTML_DD:       htmlDD(elm); break;
		case HTML_DEL:      renderPseudoElement(":before", elm); break;
		case HTML_DIV:      htmlDIV(elm); break;
		case HTML_DL:       htmlDL(elm); break;
		case HTML_DT:       htmlDT(elm); break;
		case HTML_EM:       renderPseudoElement(":before", elm); break;
		case HTML_FONT:     htmlFONT(elm); break;
		case HTML_FORM:     htmlFORM(elm); break;
		case HTML_H1://no-break
		case HTML_H2://no-break
		case HTML_H3://no-break
		case HTML_H4://no-break
		case HTML_H5://no-break
		case HTML_H6:		htmlH(elm); break;
		case HTML_HEAD:     htmlHEAD(elm); break;
		case HTML_HR:       htmlHR(elm); break;
		case HTML_HTML:     htmlHTML(elm); break;
		case HTML_I:        htmlI(elm); break;
		case HTML_IMG:      htmlIMG(elm); break;
		case HTML_INPUT:    htmlINPUT(elm); break;
		case HTML_LI:       htmlLI(elm); break;
		case HTML_LUA:      htmlLUA(elm); break;
		case HTML_META:     htmlMETA(elm); break;
		case HTML_OBJECT:   htmlOBJECT(elm); break;
		case HTML_OL:       htmlOL(elm); break;
		case HTML_OPTION:   htmlOPTION(elm); break;
		case HTML_P:        htmlP(elm); break;
		case HTML_PRE:      htmlPRE(elm); break;
		case HTML_SCRIPT:   htmlSCRIPT(elm); break;
		case HTML_SELECT:   htmlSELECT(elm); break;
		case HTML_SMALL:    renderPseudoElement(":before", elm); break;
		case HTML_SPAN:     renderPseudoElement(":before", elm); break;
		case HTML_STRONG:   renderPseudoElement(":before", elm); break;
		case HTML_STYLE:    htmlSTYLE(elm); break;
		case HTML_TABLE:    htmlTABLE(elm); break;
		case HTML_TD:       htmlTD(elm); break;
		case HTML_TEXTAREA: htmlTEXTAREA(elm); break;
		case HTML_TH:       htmlTH(elm); break;
		case HTML_TITLE:    htmlTITLE(elm); break;
		case HTML_TR:       htmlTR(elm); break;
		case HTML_U:        renderPseudoElement(":before", elm); break;
		case HTML_UL:       htmlUL(elm); break;
		default:
			renderPseudoElement(":before", elm);
			break;
		}
	}

	// ***************************************************************************
	void CGroupHTML::endElement(CHtmlElement &elm)
	{
		_CurrentHTMLElement = &elm;

		switch(elm.ID)
		{
		case HTML_A:        htmlAend(elm); break;
		case HTML_BASE:     break;
		case HTML_BODY:     renderPseudoElement(":after", elm); break;
		case HTML_BR:       break;
		case HTML_DD:       htmlDDend(elm); break;
		case HTML_DEL:      renderPseudoElement(":after", elm); break;
		case HTML_DIV:      htmlDIVend(elm); break;
		case HTML_DL:       htmlDLend(elm); break;
		case HTML_DT:       htmlDTend(elm); break;
		case HTML_EM:       renderPseudoElement(":after", elm);break;
		case HTML_FONT:     break;
		case HTML_FORM:     renderPseudoElement(":after", elm);break;
		case HTML_H1://no-break
		case HTML_H2://no-break
		case HTML_H3://no-break
		case HTML_H4://no-break
		case HTML_H5://no-break
		case HTML_H6:		htmlHend(elm); break;
		case HTML_HEAD:     htmlHEADend(elm); break;
		case HTML_HR:       break;
		case HTML_HTML:     break;
		case HTML_I:        htmlIend(elm); break;
		case HTML_IMG:      break;
		case HTML_INPUT:    break;
		case HTML_LI:       htmlLIend(elm); break;
		case HTML_LUA:      htmlLUAend(elm); break;
		case HTML_META:     break;
		case HTML_OBJECT:   htmlOBJECTend(elm); break;
		case HTML_OL:       htmlOLend(elm); break;
		case HTML_OPTION:   htmlOPTIONend(elm); break;
		case HTML_P:        htmlPend(elm); break;
		case HTML_PRE:      htmlPREend(elm); break;
		case HTML_SCRIPT:   htmlSCRIPTend(elm); break;
		case HTML_SELECT:   htmlSELECTend(elm); break;
		case HTML_SMALL:    renderPseudoElement(":after", elm);break;
		case HTML_SPAN:     renderPseudoElement(":after", elm);break;
		case HTML_STRONG:   renderPseudoElement(":after", elm);break;
		case HTML_STYLE:    htmlSTYLEend(elm); break;
		case HTML_TABLE:    htmlTABLEend(elm); break;
		case HTML_TD:       htmlTDend(elm); break;
		case HTML_TEXTAREA: htmlTEXTAREAend(elm); break;
		case HTML_TH:       htmlTHend(elm); break;
		case HTML_TITLE:    htmlTITLEend(elm); break;
		case HTML_TR:       htmlTRend(elm); break;
		case HTML_U:        renderPseudoElement(":after", elm); break;
		case HTML_UL:       htmlULend(elm); break;
		default:
			renderPseudoElement(":after", elm);
			break;
		}


		_Style.popStyle();
	}

	// ***************************************************************************
	void CGroupHTML::renderPseudoElement(const std::string &pseudo, const CHtmlElement &elm)
	{
		if (pseudo == ":before" && !elm.StyleBefore.empty())
		{
			_Style.pushStyle();
			_Style.applyStyle(elm.StyleBefore);
		}
		else if (pseudo == ":after" && !elm.StyleAfter.empty())
		{
			_Style.pushStyle();
			_Style.applyStyle(elm.StyleAfter);
		}
		else
		{
			// unknown pseudo element
			return;
		}

		// TODO: 'content' should already be tokenized in css parser as it has all the functions for that
		std::string content = trim(_Style.getStyle("content"));
		if (toLower(content) == "none" || toLower(content) == "normal")
		{
			return;
		}

		// TODO: use ucstring / ucchar as content is utf8 chars
		std::string::size_type pos = 0;
		while(pos < content.size())
		{
			std::string::size_type start;
			std::string token;
		
			// not supported
			// counter, open-quote, close-quote, no-open-quote, no-close-quote
			if (content[pos] == '"' || content[pos] == '\'')
			{
				char quote = content[pos];
				pos++;
				start = pos;
				while(pos < content.size() && content[pos] != quote)
				{
					if (content[pos] == '\\') pos++;
					pos++;
				}
				token = content.substr(start, pos - start);
				addString(ucstring::makeFromUtf8(token));

				// skip closing quote
				pos++;
			}
			else if (content[pos] == 'u' && pos < content.size() - 6 && toLower(content.substr(pos, 4)) == "url(")
			{
				// url(/path-to/image.jpg) / "Alt!"
				// url("/path to/image.jpg") / "Alt!"
				std::string tooltip;

				start = pos + 4;
				// fails if url contains ')'
				pos = content.find(")", start);
				token = trim(content.substr(start, pos - start));
				// skip ')'
				pos++;

				// scan for tooltip
				start = pos;
				while(pos < content.size() && content[pos] == ' ' && content[pos] != '/')
				{
					pos++;
				}
				if (pos < content.size() && content[pos] == '/')
				{
					// skip '/'
					pos++;

					// skip whitespace
					while(pos < content.size() && content[pos] == ' ')
					{
						pos++;
					}
					if (pos < content.size() && (content[pos] == '\'' || content[pos] == '"'))
					{
						char openQuote =  content[pos];
						pos++;
						start = pos;
						while(pos < content.size() && content[pos] != openQuote)
						{
							if (content[pos] == '\\') pos++;
							pos++;
						}
						tooltip = content.substr(start, pos - start);

						// skip closing quote
						pos++;
					}
					else
					{
						// tooltip should be quoted
						pos = start;
						tooltip.clear();
					}
				}
				else
				{
					// no tooltip
					pos = start;
				}

				if (tooltip.empty())
				{
					addImage(getId() + pseudo, token, false, _Style.Current);
				}
				else
				{
					tooltip = trimQuotes(tooltip);
					addButton(CCtrlButton::PushButton, getId() + pseudo, token, token, "", "", "", tooltip.c_str(), _Style.Current);
				}
			}
			else if (content[pos] == 'a' && pos < content.size() - 7)
			{
				// attr(title)
				start = pos + 5;
				pos = content.find(")", start);
				token = content.substr(start, pos - start);
				// skip ')'
				pos++;

				if (elm.hasAttribute(token))
				{
					addString(ucstring::makeFromUtf8(elm.getAttribute(token)));
				}
			}
			else
			{
				pos++;
			}
		}

		_Style.popStyle();
	}

	// ***************************************************************************
	void CGroupHTML::renderDOM(CHtmlElement &elm)
	{
		if (elm.Type == CHtmlElement::TEXT_NODE)
		{
			addText(elm.Value.c_str(), elm.Value.size());
		}
		else
		{
			beginElement(elm);

			std::list<CHtmlElement>::iterator it = elm.Children.begin();
			while(it != elm.Children.end())
			{
				renderDOM(*it);

				++it;
			}

			endElement(elm);
		}
	}

	// ***************************************************************************
	NLMISC_REGISTER_OBJECT(CViewBase, CGroupHTML, std::string, "html");


	// ***************************************************************************
	uint32							CGroupHTML::_GroupHtmlUIDPool= 0;
	CGroupHTML::TGroupHtmlByUIDMap	CGroupHTML::_GroupHtmlByUID;


	// ***************************************************************************
	CGroupHTML::CGroupHTML(const TCtorParam &param)
	:	CGroupScrollText(param),
		_TimeoutValue(DEFAULT_RYZOM_CONNECTION_TIMEOUT),
		_RedirectsRemaining(DEFAULT_RYZOM_REDIRECT_LIMIT),
		_CurrentHTMLElement(NULL)
	{
		// add it to map of group html created
		_GroupHtmlUID= ++_GroupHtmlUIDPool; // valid assigned Id begin to 1!
		_GroupHtmlByUID[_GroupHtmlUID]= this;

		// init
		_TrustedDomain = false;
		_ParsingLua = false;
		_LuaHrefHack = false;
		_IgnoreText = false;
		_BrowseNextTime = false;
		_PostNextTime = false;
		_Browsing = false;
		_Connecting = false;
		_CurrentViewLink = NULL;
		_CurrentViewImage = NULL;
		_Indent.clear();
		_LI = false;
		_SelectOption = false;
		_GroupListAdaptor = NULL;
		_UrlFragment.clear();
		_RefreshUrl.clear();
		_NextRefreshTime = 0.0;
		_LastRefreshTime = 0.0;
		_RenderNextTime = false;
		_WaitingForStylesheet = false;

		// Register
		CWidgetManager::getInstance()->registerClockMsgTarget(this);

		// HTML parameters
		BgColor = CRGBA::Black;
		ErrorColor = CRGBA(255, 0, 0);
		LinkColor = CRGBA(0, 0, 255);
		TextColor = CRGBA(255, 255, 255);
		H1Color = CRGBA(255, 255, 255);
		H2Color = CRGBA(255, 255, 255);
		H3Color = CRGBA(255, 255, 255);
		H4Color = CRGBA(255, 255, 255);
		H5Color = CRGBA(255, 255, 255);
		H6Color = CRGBA(255, 255, 255);
		ErrorColorGlobalColor = false;
		LinkColorGlobalColor = false;
		TextColorGlobalColor = false;
		H1ColorGlobalColor = false;
		H2ColorGlobalColor = false;
		H3ColorGlobalColor = false;
		H4ColorGlobalColor = false;
		H5ColorGlobalColor = false;
		H6ColorGlobalColor = false;
		TextFontSize = 9;
		H1FontSize = 18;
		H2FontSize = 15;
		H3FontSize = 12;
		H4FontSize = 9;
		H5FontSize = 9;
		H6FontSize = 9;
		LIBeginSpace = 4;
		ULBeginSpace = 12;
		PBeginSpace	 = 12;
		TDBeginSpace = 0;
		LIIndent = -10;
		ULIndent = 30;
		LineSpaceFontFactor = 0.5f;
		DefaultButtonGroup =			"html_text_button";
		DefaultFormTextGroup =			"edit_box_widget";
		DefaultFormTextAreaGroup =		"edit_box_widget_multiline";
		DefaultFormSelectGroup =		"html_form_select_widget";
		DefaultFormSelectBoxMenuGroup =	"html_form_select_box_menu_widget";
		DefaultCheckBoxBitmapNormal =	"checkbox_normal.tga";
		DefaultCheckBoxBitmapPushed =	"checkbox_pushed.tga";
		DefaultCheckBoxBitmapOver =		"checkbox_over.tga";
		DefaultRadioButtonBitmapNormal = "w_radiobutton.png";
		DefaultRadioButtonBitmapPushed = "w_radiobutton_pushed.png";
		DefaultBackgroundBitmapView =	"bg";
		clearContext();

		MultiCurl = curl_multi_init();
#ifdef CURLMOPT_MAX_HOST_CONNECTIONS
		if (MultiCurl)
		{
			// added in libcurl 7.30.0
			curl_multi_setopt(MultiCurl, CURLMOPT_MAX_HOST_CONNECTIONS, options.curlMaxConnections);
			curl_multi_setopt(MultiCurl, CURLMOPT_PIPELINING, 1);
		}
#endif
		RunningCurls = 0;
		_CurlWWW = NULL;

		initImageDownload();
		initBnpDownload();
		initLibWWW();
	}

	// ***************************************************************************

	CGroupHTML::~CGroupHTML()
	{
		//releaseImageDownload();

		// TestYoyo
		//nlinfo("** CGroupHTML Destroy: %x, %s, uid%d", this, _Id.c_str(), _GroupHtmlUID);

		/*	Erase from map of Group HTML (thus requestTerminated() callback won't be called)
			Do it first, just because don't want requestTerminated() to be called while I'm destroying
			(useless and may be dangerous)
		*/
		_GroupHtmlByUID.erase(_GroupHtmlUID);

		// stop browsing
		stopBrowse (); // NB : we don't call updateRefreshButton here, because :
					   // 1) it is useless,
					   // 2) it crashed before when it called getElementFromId (that didn't work when a master group was being removed...). Btw it should work now
					   //     this is why the call to 'updateRefreshButton' has been removed from stopBrowse

		clearContext();
		releaseDownloads();

		if (_CurlWWW)
			delete _CurlWWW;

		if(MultiCurl)
			curl_multi_cleanup(MultiCurl);
	}

	std::string CGroupHTML::getProperty( const std::string &name ) const
	{
		if( name == "url" )
		{
			return _URL;
		}
		else
		if( name == "title_prefix" )
		{
			return _TitlePrefix.toString();
		}
		else
		if( name == "background_color" )
		{
			return toString( BgColor );
		}
		else
		if( name == "error_color" )
		{
			return toString( ErrorColor );
		}
		else
		if( name == "link_color" )
		{
			return toString( LinkColor );
		}
		else
		if( name == "h1_color" )
		{
			return toString( H1Color );
		}
		else
		if( name == "h2_color" )
		{
			return toString( H2Color );
		}
		else
		if( name == "h3_color" )
		{
			return toString( H3Color );
		}
		else
		if( name == "h4_color" )
		{
			return toString( H4Color );
		}
		else
		if( name == "h5_color" )
		{
			return toString( H5Color );
		}
		else
		if( name == "h6_color" )
		{
			return toString( H6Color );
		}
		else
		if( name == "error_color_global_color" )
		{			
			return toString( ErrorColorGlobalColor );
		}
		else
		if( name == "link_color_global_color" )
		{			
			return toString( LinkColorGlobalColor );
		}
		else
		if( name == "text_color_global_color" )
		{
			return toString( TextColorGlobalColor );
		}
		else
		if( name == "h1_color_global_color" )
		{			
			return toString( H1ColorGlobalColor );
		}
		else
		if( name == "h2_color_global_color" )
		{			
			return toString( H2ColorGlobalColor );
		}
		else
		if( name == "h3_color_global_color" )
		{			
			return toString( H3ColorGlobalColor );
		}
		else
		if( name == "h4_color_global_color" )
		{			
			return toString( H4ColorGlobalColor );
		}
		else
		if( name == "h5_color_global_color" )
		{			
			return toString( H5ColorGlobalColor );
		}
		else
		if( name == "h6_color_global_color" )
		{			
			return toString( H6ColorGlobalColor );
		}
		else
		if( name == "text_font_size" )
		{			
			return toString( TextFontSize );
		}
		else
		if( name == "h1_font_size" )
		{			
			return toString( H1FontSize );
		}
		else
		if( name == "h2_font_size" )
		{			
			return toString( H2FontSize );
		}
		else
		if( name == "h3_font_size" )
		{			
			return toString( H3FontSize );
		}
		else
		if( name == "h4_font_size" )
		{			
			return toString( H4FontSize );
		}
		else
		if( name == "h5_font_size" )
		{			
			return toString( H5FontSize );
		}
		else
		if( name == "h6_font_size" )
		{			
			return toString( H6FontSize );
		}
		else
		if( name == "td_begin_space" )
		{
			return toString( TDBeginSpace );
		}
		else
		if( name == "paragraph_begin_space" )
		{
			return toString( PBeginSpace );
		}
		else
		if( name == "li_begin_space" )
		{
			return toString( LIBeginSpace );
		}
		else
		if( name == "ul_begin_space" )
		{
			return toString( ULBeginSpace );
		}
		else
		if( name == "li_indent" )
		{
			return toString( LIIndent );
		}
		else
		if( name == "ul_indent" )
		{
			return toString( ULIndent );
		}
		else
		if( name == "multi_line_space_factor" )
		{
			return toString( LineSpaceFontFactor );
		}
		else
		if( name == "form_text_area_group" )
		{
			return DefaultFormTextGroup;
		}
		else
		if( name == "form_select_group" )
		{
			return DefaultFormSelectGroup;
		}
		else
		if( name == "checkbox_bitmap_normal" )
		{
			return DefaultCheckBoxBitmapNormal;
		}
		else
		if( name == "checkbox_bitmap_pushed" )
		{
			return DefaultCheckBoxBitmapPushed;
		}
		else
		if( name == "checkbox_bitmap_over" )
		{
			return DefaultCheckBoxBitmapOver;
		}
		else
		if( name == "radiobutton_bitmap_normal" )
		{
			return DefaultRadioButtonBitmapNormal;
		}
		else
		if( name == "radiobutton_bitmap_pushed" )
		{
			return DefaultRadioButtonBitmapPushed;
		}
		else
		if( name == "radiobutton_bitmap_over" )
		{
			return DefaultRadioButtonBitmapOver;
		}
		else
		if( name == "background_bitmap_view" )
		{
			return DefaultBackgroundBitmapView;
		}
		else
		if( name == "home" )
		{
			return Home;
		}
		else
		if( name == "browse_next_time" )
		{
			return toString( _BrowseNextTime );
		}
		else
		if( name == "browse_tree" )
		{
			return _BrowseTree;
		}
		else
		if( name == "browse_undo" )
		{
			return _BrowseUndoButton;
		}
		else
		if( name == "browse_redo" )
		{
			return _BrowseRedoButton;
		}
		else
		if( name == "browse_refresh" )
		{
			return _BrowseRefreshButton;
		}
		else
		if( name == "timeout" )
		{
			return toString( _TimeoutValue );
		}
		else
			return CGroupScrollText::getProperty( name );
	}

	void CGroupHTML::setProperty( const std::string &name, const std::string &value )
	{
		if( name == "url" )
		{
			_URL = value;
			return;
		}
		else
		if( name == "title_prefix" )
		{
			_TitlePrefix = value;
			return;
		}
		else
		if( name == "background_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				BgColor = c;
			return;
		}
		else
		if( name == "error_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				ErrorColor = c;
			return;
		}
		else
		if( name == "link_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				LinkColor = c;
			return;
		}
		else
		if( name == "h1_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				H1Color = c;
			return;
		}
		else
		if( name == "h2_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				H2Color = c;
			return;
		}
		else
		if( name == "h3_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				H3Color = c;
			return;
		}
		else
		if( name == "h4_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				H4Color = c;
			return;
		}
		else
		if( name == "h5_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				H5Color = c;
			return;
		}
		else
		if( name == "h6_color" )
		{
			CRGBA c;
			if( fromString( value, c ) )
				H6Color = c;
			return;
		}
		else
		if( name == "error_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				ErrorColorGlobalColor = b;
			return;
		}
		else
		if( name == "link_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				LinkColorGlobalColor = b;
			return;
		}
		else
		if( name == "text_color_global_color" )
		{
			bool b;
			if( fromString( value, b ) )
				TextColorGlobalColor = b;
			return;
		}
		else
		if( name == "h1_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				H1ColorGlobalColor = b;
			return;
		}
		else
		if( name == "h2_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				H2ColorGlobalColor = b;
			return;
		}
		else
		if( name == "h3_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				H3ColorGlobalColor = b;
			return;
		}
		else
		if( name == "h4_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				H4ColorGlobalColor = b;
			return;
		}
		else
		if( name == "h5_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				H5ColorGlobalColor = b;
			return;
		}
		else
		if( name == "h6_color_global_color" )
		{			
			bool b;
			if( fromString( value, b ) )
				H6ColorGlobalColor = b;
			return;
		}
		else
		if( name == "text_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				TextFontSize = i;
			return;
		}
		else
		if( name == "h1_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				H1FontSize = i;
			return;
		}
		else
		if( name == "h2_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				H2FontSize = i;
			return;
		}
		else
		if( name == "h3_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				H3FontSize = i;
			return;
		}
		else
		if( name == "h4_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				H4FontSize = i;
			return;
		}
		else
		if( name == "h5_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				H5FontSize = i;
			return;
		}
		else
		if( name == "h6_font_size" )
		{			
			uint i;
			if( fromString( value, i ) )
				H6FontSize = i;
			return;
		}
		else
		if( name == "td_begin_space" )
		{
			uint i;
			if( fromString( value, i ) )
				TDBeginSpace = i;
			return;
		}
		else
		if( name == "paragraph_begin_space" )
		{
			uint i;
			if( fromString( value, i ) )
				PBeginSpace = i;
			return;
		}
		else
		if( name == "li_begin_space" )
		{
			uint i;
			if( fromString( value, i ) )
				LIBeginSpace = i;
			return;
		}
		else
		if( name == "ul_begin_space" )
		{
			uint i;
			if( fromString( value, i ) )
				ULBeginSpace = i;
			return;
		}
		else
		if( name == "li_indent" )
		{
			uint i;
			if( fromString( value, i ) )
				LIIndent = i;
			return;
		}
		else
		if( name == "ul_indent" )
		{
			uint i;
			if( fromString( value, i ) )
				ULIndent = i;
			return;
		}
		else
		if( name == "multi_line_space_factor" )
		{
			float f;
			if( fromString( value, f ) )
				LineSpaceFontFactor = f;
			return;
		}
		else
		if( name == "form_text_area_group" )
		{
			DefaultFormTextGroup = value;
			return;
		}
		else
		if( name == "form_select_group" )
		{
			DefaultFormSelectGroup = value;
			return;
		}
		else
		if( name == "checkbox_bitmap_normal" )
		{
			DefaultCheckBoxBitmapNormal = value;
			return;
		}
		else
		if( name == "checkbox_bitmap_pushed" )
		{
			DefaultCheckBoxBitmapPushed = value;
			return;
		}
		else
		if( name == "checkbox_bitmap_over" )
		{
			DefaultCheckBoxBitmapOver = value;
			return;
		}
		else
		if( name == "radiobutton_bitmap_normal" )
		{
			DefaultRadioButtonBitmapNormal = value;
			return;
		}
		else
		if( name == "radiobutton_bitmap_pushed" )
		{
			DefaultRadioButtonBitmapPushed = value;
			return;
		}
		else
		if( name == "radiobutton_bitmap_over" )
		{
			DefaultRadioButtonBitmapOver = value;
			return;
		}
		else
		if( name == "background_bitmap_view" )
		{
			DefaultBackgroundBitmapView = value;
			return;
		}
		else
		if( name == "home" )
		{
			Home = value;
			return;
		}
		else
		if( name == "browse_next_time" )
		{
			bool b;
			if( fromString( value, b ) )
				_BrowseNextTime = b;
			return;
		}
		else
		if( name == "browse_tree" )
		{
			_BrowseTree = value;
			return;
		}
		else
		if( name == "browse_undo" )
		{
			_BrowseUndoButton = value;
			return;
		}
		else
		if( name == "browse_redo" )
		{
			_BrowseRedoButton = value;
			return;
		}
		else
		if( name == "browse_refresh" )
		{
			_BrowseRefreshButton = value;
			return;
		}
		else
		if( name == "timeout" )
		{
			double d;
			if( fromString( value, d ) )
				_TimeoutValue = d;
			return;
		}
		else
			CGroupScrollText::setProperty( name, value );
	}

	xmlNodePtr CGroupHTML::serialize( xmlNodePtr parentNode, const char *type ) const
	{
		xmlNodePtr node = CGroupScrollText::serialize( parentNode, type );
		if( node == NULL )
			return NULL;

		xmlSetProp( node, BAD_CAST "type", BAD_CAST "html" );
		xmlSetProp( node, BAD_CAST "url", BAD_CAST _URL.c_str() );
		xmlSetProp( node, BAD_CAST "title_prefix", BAD_CAST _TitlePrefix.toString().c_str() );
		xmlSetProp( node, BAD_CAST "background_color", BAD_CAST toString( BgColor ).c_str() );
		xmlSetProp( node, BAD_CAST "error_color", BAD_CAST toString( ErrorColor ).c_str() );
		xmlSetProp( node, BAD_CAST "link_color", BAD_CAST toString( LinkColor ).c_str() );
		xmlSetProp( node, BAD_CAST "background_color", BAD_CAST toString( BgColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h1_color", BAD_CAST toString( H1Color ).c_str() );
		xmlSetProp( node, BAD_CAST "h2_color", BAD_CAST toString( H2Color ).c_str() );
		xmlSetProp( node, BAD_CAST "h3_color", BAD_CAST toString( H3Color ).c_str() );
		xmlSetProp( node, BAD_CAST "h4_color", BAD_CAST toString( H4Color ).c_str() );
		xmlSetProp( node, BAD_CAST "h5_color", BAD_CAST toString( H5Color ).c_str() );
		xmlSetProp( node, BAD_CAST "h6_color", BAD_CAST toString( H6Color ).c_str() );
		
		xmlSetProp( node, BAD_CAST "error_color_global_color",
			BAD_CAST toString( ErrorColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "link_color_global_color",
			BAD_CAST toString( LinkColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "text_color_global_color",
			BAD_CAST toString( TextColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h1_color_global_color",
			BAD_CAST toString( H1ColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h2_color_global_color",
			BAD_CAST toString( H2ColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h3_color_global_color",
			BAD_CAST toString( H3ColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h4_color_global_color",
			BAD_CAST toString( H4ColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h5_color_global_color",
			BAD_CAST toString( H5ColorGlobalColor ).c_str() );
		xmlSetProp( node, BAD_CAST "h6_color_global_color",
			BAD_CAST toString( H6ColorGlobalColor ).c_str() );

		xmlSetProp( node, BAD_CAST "text_font_size", BAD_CAST toString( TextFontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "h1_font_size", BAD_CAST toString( H1FontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "h2_font_size", BAD_CAST toString( H2FontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "h3_font_size", BAD_CAST toString( H3FontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "h4_font_size", BAD_CAST toString( H4FontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "h5_font_size", BAD_CAST toString( H5FontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "h6_font_size", BAD_CAST toString( H6FontSize ).c_str() );
		xmlSetProp( node, BAD_CAST "td_begin_space", BAD_CAST toString( TDBeginSpace ).c_str() );
		xmlSetProp( node, BAD_CAST "paragraph_begin_space", BAD_CAST toString( PBeginSpace ).c_str() );
		xmlSetProp( node, BAD_CAST "li_begin_space", BAD_CAST toString( LIBeginSpace ).c_str() );
		xmlSetProp( node, BAD_CAST "ul_begin_space", BAD_CAST toString( ULBeginSpace ).c_str() );
		xmlSetProp( node, BAD_CAST "li_indent", BAD_CAST toString( LIIndent ).c_str() );
		xmlSetProp( node, BAD_CAST "ul_indent", BAD_CAST toString( ULIndent ).c_str() );
		xmlSetProp( node, BAD_CAST "multi_line_space_factor", BAD_CAST toString( LineSpaceFontFactor ).c_str() );
		xmlSetProp( node, BAD_CAST "form_text_area_group", BAD_CAST DefaultFormTextGroup.c_str() );
		xmlSetProp( node, BAD_CAST "form_select_group", BAD_CAST DefaultFormSelectGroup.c_str() );
		xmlSetProp( node, BAD_CAST "checkbox_bitmap_normal", BAD_CAST DefaultCheckBoxBitmapNormal.c_str() );
		xmlSetProp( node, BAD_CAST "checkbox_bitmap_pushed", BAD_CAST DefaultCheckBoxBitmapPushed.c_str() );
		xmlSetProp( node, BAD_CAST "checkbox_bitmap_over", BAD_CAST DefaultCheckBoxBitmapOver.c_str() );
		xmlSetProp( node, BAD_CAST "radiobutton_bitmap_normal", BAD_CAST DefaultRadioButtonBitmapNormal.c_str() );
		xmlSetProp( node, BAD_CAST "radiobutton_bitmap_pushed", BAD_CAST DefaultRadioButtonBitmapPushed.c_str() );
		xmlSetProp( node, BAD_CAST "radiobutton_bitmap_over", BAD_CAST DefaultRadioButtonBitmapOver.c_str() );
		xmlSetProp( node, BAD_CAST "background_bitmap_view", BAD_CAST DefaultBackgroundBitmapView.c_str() );
		xmlSetProp( node, BAD_CAST "home", BAD_CAST Home.c_str() );
		xmlSetProp( node, BAD_CAST "browse_next_time", BAD_CAST toString( _BrowseNextTime ).c_str() );
		xmlSetProp( node, BAD_CAST "browse_tree", BAD_CAST _BrowseTree.c_str() );
		xmlSetProp( node, BAD_CAST "browse_undo", BAD_CAST _BrowseUndoButton.c_str() );
		xmlSetProp( node, BAD_CAST "browse_redo", BAD_CAST _BrowseRedoButton.c_str() );
		xmlSetProp( node, BAD_CAST "browse_refresh", BAD_CAST _BrowseRefreshButton.c_str() );
		xmlSetProp( node, BAD_CAST "timeout", BAD_CAST toString( _TimeoutValue ).c_str() );

		return node;
	}

	// ***************************************************************************

	bool CGroupHTML::parse(xmlNodePtr cur,CInterfaceGroup *parentGroup)
	{
		nlassert( CWidgetManager::getInstance()->isClockMsgTarget(this));


		if(!CGroupScrollText::parse(cur, parentGroup))
			return false;

		// TestYoyo
		//nlinfo("** CGroupHTML parsed Ok: %x, %s, %s, uid%d", this, _Id.c_str(), typeid(this).name(), _GroupHtmlUID);

		CXMLAutoPtr ptr;

		// Get the url
		ptr = xmlGetProp (cur, (xmlChar*)"url");
		if (ptr)
			_URL = (const char*)ptr;

		// Bkup default for undo/redo
		_AskedUrl= _URL;

		ptr = xmlGetProp (cur, (xmlChar*)"title_prefix");
		if (ptr)
			_TitlePrefix = CI18N::get((const char*)ptr);

		// Parameters
		ptr = xmlGetProp (cur, (xmlChar*)"background_color");
		if (ptr)
			BgColor = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"error_color");
		if (ptr)
			ErrorColor = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"link_color");
		if (ptr)
			LinkColor = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"text_color");
		if (ptr)
			TextColor = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h1_color");
		if (ptr)
			H1Color = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h2_color");
		if (ptr)
			H2Color = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h3_color");
		if (ptr)
			H3Color = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h4_color");
		if (ptr)
			H4Color = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h5_color");
		if (ptr)
			H5Color = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h6_color");
		if (ptr)
			H6Color = convertColor(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"error_color_global_color");
		if (ptr)
			ErrorColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"link_color_global_color");
		if (ptr)
			LinkColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"text_color_global_color");
		if (ptr)
			TextColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h1_color_global_color");
		if (ptr)
			H1ColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h2_color_global_color");
		if (ptr)
			H2ColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h3_color_global_color");
		if (ptr)
			H3ColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h4_color_global_color");
		if (ptr)
			H4ColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h5_color_global_color");
		if (ptr)
			H5ColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"h6_color_global_color");
		if (ptr)
			H6ColorGlobalColor = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"text_font_size");
		if (ptr)
			fromString((const char*)ptr, TextFontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"h1_font_size");
		if (ptr)
			fromString((const char*)ptr, H1FontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"h2_font_size");
		if (ptr)
			fromString((const char*)ptr, H2FontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"h3_font_size");
		if (ptr)
			fromString((const char*)ptr, H3FontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"h4_font_size");
		if (ptr)
			fromString((const char*)ptr, H4FontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"h5_font_size");
		if (ptr)
			fromString((const char*)ptr, H5FontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"h6_font_size");
		if (ptr)
			fromString((const char*)ptr, H6FontSize);
		ptr = xmlGetProp (cur, (xmlChar*)"td_begin_space");
		if (ptr)
			fromString((const char*)ptr, TDBeginSpace);
		ptr = xmlGetProp (cur, (xmlChar*)"paragraph_begin_space");
		if (ptr)
			fromString((const char*)ptr, PBeginSpace);
		ptr = xmlGetProp (cur, (xmlChar*)"li_begin_space");
		if (ptr)
			fromString((const char*)ptr, LIBeginSpace);
		ptr = xmlGetProp (cur, (xmlChar*)"ul_begin_space");
		if (ptr)
			fromString((const char*)ptr, ULBeginSpace);
		ptr = xmlGetProp (cur, (xmlChar*)"li_indent");
		if (ptr)
			fromString((const char*)ptr, LIIndent);
		ptr = xmlGetProp (cur, (xmlChar*)"ul_indent");
		if (ptr)
			fromString((const char*)ptr, ULIndent);
		ptr = xmlGetProp (cur, (xmlChar*)"multi_line_space_factor");
		if (ptr)
			fromString((const char*)ptr, LineSpaceFontFactor);
		ptr = xmlGetProp (cur, (xmlChar*)"form_text_group");
		if (ptr)
			DefaultFormTextGroup = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"form_text_area_group");
		if (ptr)
			DefaultFormTextAreaGroup = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"form_select_group");
		if (ptr)
			DefaultFormSelectGroup = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"checkbox_bitmap_normal");
		if (ptr)
			DefaultCheckBoxBitmapNormal = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"checkbox_bitmap_pushed");
		if (ptr)
			DefaultCheckBoxBitmapPushed = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"checkbox_bitmap_over");
		if (ptr)
			DefaultCheckBoxBitmapOver = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"radiobutton_bitmap_normal");
		if (ptr)
			DefaultRadioButtonBitmapNormal = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"radiobutton_bitmap_pushed");
		if (ptr)
			DefaultRadioButtonBitmapPushed = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"radiobutton_bitmap_over");
		if (ptr)
			DefaultRadioButtonBitmapOver = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"background_bitmap_view");
		if (ptr)
			DefaultBackgroundBitmapView = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"home");
		if (ptr)
			Home = (const char*)(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"browse_next_time");
		if (ptr)
			_BrowseNextTime = convertBool(ptr);
		ptr = xmlGetProp (cur, (xmlChar*)"browse_tree");
		if(ptr)
			_BrowseTree = (const char*)ptr;
		ptr = xmlGetProp (cur, (xmlChar*)"browse_undo");
		if(ptr)
			_BrowseUndoButton= (const char*)ptr;
		ptr = xmlGetProp (cur, (xmlChar*)"browse_redo");
		if(ptr)
			_BrowseRedoButton = (const char*)ptr;
		ptr = xmlGetProp (cur, (xmlChar*)"browse_refresh");
		if(ptr)
			_BrowseRefreshButton = (const char*)ptr;
		ptr = xmlGetProp (cur, (xmlChar*)"timeout");
		if(ptr)
			fromString((const char*)ptr, _TimeoutValue);

		return true;
	}

	// ***************************************************************************

	bool CGroupHTML::handleEvent (const NLGUI::CEventDescriptor& eventDesc)
	{
		bool traited = false;

		if (eventDesc.getType() == NLGUI::CEventDescriptor::mouse)
		{
			const NLGUI::CEventDescriptorMouse &mouseEvent = (const NLGUI::CEventDescriptorMouse &)eventDesc;
			if (mouseEvent.getEventTypeExtended() == NLGUI::CEventDescriptorMouse::mousewheel)
			{
				// Check if mouse wheel event was on any of multiline select box widgets
				// Must do this before CGroupScrollText
				for (uint i=0; i<_Forms.size() && !traited; i++)
				{
					for (uint j=0; j<_Forms[i].Entries.size() && !traited; j++)
					{
						if (_Forms[i].Entries[j].SelectBox)
						{
							if (_Forms[i].Entries[j].SelectBox->handleEvent(eventDesc))
							{
								traited = true;
								break;
							}
						}
					}
				}
			}
		}

		if (!traited)
			traited = CGroupScrollText::handleEvent (eventDesc);

		if (eventDesc.getType() == NLGUI::CEventDescriptor::system)
		{
			const NLGUI::CEventDescriptorSystem &systemEvent = (const NLGUI::CEventDescriptorSystem &) eventDesc;
			if (systemEvent.getEventTypeExtended() == NLGUI::CEventDescriptorSystem::clocktick)
			{
				// Handle now
				handle ();
			}
			if (systemEvent.getEventTypeExtended() == NLGUI::CEventDescriptorSystem::activecalledonparent)
			{
				if (!((NLGUI::CEventDescriptorActiveCalledOnParent &) systemEvent).getActive())
				{
					// stop refresh when window gets hidden
					_NextRefreshTime = 0;
				}
			}
		}
		return traited;
	}

	// ***************************************************************************

	void CGroupHTML::endParagraph()
	{
		_Paragraph = NULL;

		paragraphChange ();
	}

	// ***************************************************************************

	void CGroupHTML::newParagraph(uint beginSpace)
	{
		// Add a new paragraph
		CGroupParagraph *newParagraph = new CGroupParagraph(CViewBase::TCtorParam());
		newParagraph->setResizeFromChildH(true);

		newParagraph->setMarginLeft(getIndent());

		// Add to the group
		addHtmlGroup (newParagraph, beginSpace);
		_Paragraph = newParagraph;

		paragraphChange ();
	}

	// ***************************************************************************

	void CGroupHTML::browse(const char *url)
	{
		// modify undo/redo
		pushUrlUndoRedo(url);

		// do the browse, with no undo/redo
		doBrowse(url);
	}

	// ***************************************************************************
	void CGroupHTML::refresh()
	{
		if (!_URL.empty())
			doBrowse(_URL.c_str(), true);
	}

	// ***************************************************************************
	void CGroupHTML::doBrowse(const char *url, bool force)
	{
		// Stop previous browse
		if (_Browsing)
		{
			// Clear all the context
			clearContext();

			_Browsing = false;
			updateRefreshButton();

	#ifdef LOG_DL
			nlwarning("(%s) *** ALREADY BROWSING, break first", _Id.c_str());
	#endif
		}

	#ifdef LOG_DL
		nlwarning("(%s) Browsing URL : '%s'", _Id.c_str(), url);
	#endif


		CUrlParser uri(url);
		if (!uri.hash.empty())
		{
			// Anchor to scroll after page has loaded
			_UrlFragment = uri.hash;

			uri.inherit(_DocumentUrl);
			uri.hash.clear();

			// compare urls and see if we only navigating to new anchor
			if (!force && _DocumentUrl == uri.toString())
			{
				// scroll happens in updateCoords()
				invalidateCoords();
				return;
			}
		}
		else
			_UrlFragment.clear();

		// go
		_URL = uri.toString();
		_Connecting = false;
		_BrowseNextTime = true;

		// if a BrowseTree is bound to us, try to select the node that opens this URL (auto-locate)
		if(!_BrowseTree.empty())
		{
			CGroupTree	*groupTree=dynamic_cast<CGroupTree*>(CWidgetManager::getInstance()->getElementFromId(_BrowseTree));
			if(groupTree)
			{
				string	nodeId= selectTreeNodeRecurs(groupTree->getRootNode(), url);
				// select the node
				if(!nodeId.empty())
				{
					groupTree->selectNodeById(nodeId);
				}
			}
		}
	}

	// ***************************************************************************

	void CGroupHTML::browseError (const char *msg)
	{
		// Get the list group from CGroupScrollText
		removeContent();
		newParagraph(0);
		CViewText *viewText = new CViewText ("", (string("Error : ")+msg).c_str());
		viewText->setColor (ErrorColor);
		viewText->setModulateGlobalColor(ErrorColorGlobalColor);
		viewText->setMultiLine (true);
		getParagraph()->addChild (viewText);
		if(!_TitlePrefix.empty())
			setTitle (_TitlePrefix);

		stopBrowse ();
		updateRefreshButton();
	}

	// ***************************************************************************

	bool CGroupHTML::isBrowsing()
	{
		return _Browsing;
	}


	void CGroupHTML::stopBrowse ()
	{
	#ifdef LOG_DL
		nlwarning("*** STOP BROWSE (%s)", _Id.c_str());
	#endif

		// Clear all the context
		clearContext();

		_Browsing = false;

		requestTerminated();
	}

	// ***************************************************************************

	void CGroupHTML::updateCoords()
	{
		CGroupScrollText::updateCoords();

		// all elements are in their correct place, tell scrollbar to scroll to anchor
		if (!_Browsing && !_UrlFragment.empty())
		{
			doBrowseAnchor(_UrlFragment);
			_UrlFragment.clear();
		}
	}

	// ***************************************************************************

	bool CGroupHTML::translateChar(ucchar &output, ucchar input, ucchar lastCharParam) const
	{
		// Keep this char ?
		bool keep = true;

		switch (input)
		{
			// Return / tab only in <PRE> mode
		case '\t':
		case '\n':
			{
				// Get the last char
				ucchar lastChar = lastCharParam;
				if (lastChar == 0)
					lastChar = getLastChar();
				keep = ((lastChar != (ucchar)' ') &&
						(lastChar != 0)) || getPRE() || (_CurrentViewImage && (lastChar == 0));
				if(!getPRE())
					input = ' ';
			}
			break;
		case ' ':
			{
				// Get the last char
				ucchar lastChar = lastCharParam;
				if (lastChar == 0)
					lastChar = getLastChar();
				keep = ((lastChar != (ucchar)' ') &&
						(lastChar != (ucchar)'\n') &&
						(lastChar != 0)) || getPRE() || (_CurrentViewImage && (lastChar == 0));
			}
			break;
		case 0xd:
			keep = false;
			break;
		}

		if (keep)
		{
			output = input;
		}

		return keep;
	}

	// ***************************************************************************

	void CGroupHTML::registerAnchor(CInterfaceElement* elm)
	{
		if (!_AnchorName.empty())
		{
			for(uint32 i=0; i <  _AnchorName.size(); ++i)
			{
				// filter out duplicates and register only first
				if (!_AnchorName[i].empty() && _Anchors.count(_AnchorName[i]) == 0)
				{
					_Anchors[_AnchorName[i]] = elm;
				}
			}

			_AnchorName.clear();
		}
	}

	// ***************************************************************************

	void CGroupHTML::addString(const ucstring &str)
	{
		ucstring tmpStr = str;

		if (_Localize)
		{
			string	_str = tmpStr.toString();
			string::size_type	p = _str.find('#');
			if (p == string::npos)
			{
				tmpStr = CI18N::get(_str);
			}
			else
			{
				string	cmd = _str.substr(0, p);
				string	arg = _str.substr(p+1);

				if (cmd == "date")
				{
					uint	year, month, day;
					sscanf(arg.c_str(), "%d/%d/%d", &year, &month, &day);
					tmpStr = CI18N::get( "uiMFIDate");

					year += (year > 70 ? 1900 : 2000);

					strFindReplace(tmpStr, "%year", toString("%d", year) );
					strFindReplace(tmpStr, "%month", CI18N::get(toString("uiMonth%02d", month)) );
					strFindReplace(tmpStr, "%day", toString("%d", day) );
				}
				else
				{
					tmpStr = arg;
				}
			}
		}

		// In title ?
		if (_Title)
		{
			_TitleString += tmpStr;
		}
		else if (_TextArea)
		{
			_TextAreaContent += tmpStr;
		}
		else if (_Object)
		{
			_ObjectScript += tmpStr.toString();
		}
		else if (_SelectOption)
		{
			if (!(_Forms.empty()))
			{
				if (!_Forms.back().Entries.empty())
				{
					_SelectOptionStr += tmpStr;
				}
			}
		}
		else
		{
			// In a paragraph ?
			if (!_Paragraph)
			{
				newParagraph (0);
				paragraphChange ();
			}

			CStyleParams &style = _Style.Current;

			// Text added ?
			bool added = false;
			bool embolden = style.FontWeight >= FONT_WEIGHT_BOLD;

			// Number of child in this paragraph
			if (_CurrentViewLink)
			{
				bool skipLine = !_CurrentViewLink->getText().empty() && *(_CurrentViewLink->getText().rbegin()) == (ucchar) '\n';
				bool sameShadow = style.TextShadow.Enabled && _CurrentViewLink->getShadow();
				if (sameShadow && style.TextShadow.Enabled)
				{
					sint sx, sy;
					_CurrentViewLink->getShadowOffset(sx, sy);
					sameShadow = (style.TextShadow.Color == _CurrentViewLink->getShadowColor());
					sameShadow = sameShadow && (style.TextShadow.Outline == _CurrentViewLink->getShadowOutline());
					sameShadow = sameShadow && (style.TextShadow.X == sx) && (style.TextShadow.Y == sy);
				}
				// Compatible with current parameters ?
				if (!skipLine && sameShadow &&
					(style.TextColor == _CurrentViewLink->getColor()) &&
					(style.FontFamily == _CurrentViewLink->getFontName()) &&
					(style.FontSize == (uint)_CurrentViewLink->getFontSize()) &&
					(style.Underlined == _CurrentViewLink->getUnderlined()) &&
					(style.StrikeThrough == _CurrentViewLink->getStrikeThrough()) &&
					(embolden == _CurrentViewLink->getEmbolden()) &&
					(style.FontOblique == _CurrentViewLink->getOblique()) &&
					(getLink() == _CurrentViewLink->Link) &&
					(style.GlobalColor == _CurrentViewLink->getModulateGlobalColor()))
				{
					// Concat the text
					_CurrentViewLink->setText(_CurrentViewLink->getText()+tmpStr);
					_CurrentViewLink->invalidateContent();
					added = true;
				}
			}

			// Not added ?
			if (!added)
			{
				if (getA() && string(getLinkClass()) == "ryzom-ui-button")
				{
					string buttonTemplate = DefaultButtonGroup;
					// Action handler parameters : "name=group_html_id|form=id_of_the_form|submit_button=button_name"
					string param = "name=" + this->_Id + "|url=" + getLink();

					typedef pair<string, string> TTmplParam;
					vector<TTmplParam> tmplParams;
					tmplParams.push_back(TTmplParam("id", ""));
					tmplParams.push_back(TTmplParam("onclick", "browse"));
					tmplParams.push_back(TTmplParam("onclick_param", param));
					tmplParams.push_back(TTmplParam("active", "true"));
					CInterfaceGroup *buttonGroup = CWidgetManager::getInstance()->getParser()->createGroupInstance(buttonTemplate, _Paragraph->getId(), tmplParams);
					if (buttonGroup)
					{

						// Add the ctrl button
						CCtrlTextButton *ctrlButton = dynamic_cast<CCtrlTextButton*>(buttonGroup->getCtrl("button"));
						if (!ctrlButton) ctrlButton = dynamic_cast<CCtrlTextButton*>(buttonGroup->getCtrl("b"));
						if (ctrlButton)
						{
							ctrlButton->setModulateGlobalColorAll (false);

							// Translate the tooltip
							ctrlButton->setDefaultContextHelp(ucstring::makeFromUtf8(getLinkTitle()));
							ctrlButton->setText(tmpStr);

							setTextButtonStyle(ctrlButton, style);
						}
						getParagraph()->addChild (buttonGroup);
						paragraphChange ();
					}
		
				}
				else
				{
					CViewLink *newLink = new CViewLink(CViewBase::TCtorParam());
					if (getA())
					{
						newLink->Link = getLink();
						newLink->LinkTitle = getLinkTitle();
						if (!newLink->Link.empty())
						{
							newLink->setHTMLView (this);

							newLink->setActionOnLeftClick("browse");
							newLink->setParamsOnLeftClick("name=" + getId() + "|url=" + newLink->Link);
						}
					}
					newLink->setText(tmpStr);
					newLink->setMultiLineSpace((uint)((float)(style.FontSize)*LineSpaceFontFactor));
					newLink->setMultiLine(true);
					newLink->setModulateGlobalColor(style.GlobalColor);
					setTextStyle(newLink, style);
					// newLink->setLineAtBottom (true);

					registerAnchor(newLink);

					if (getA() && !newLink->Link.empty())
					{
						getParagraph()->addChildLink(newLink);
					}
					else
					{
						getParagraph()->addChild(newLink);
					}
					paragraphChange ();
				}
			}
		}
	}

	// ***************************************************************************

	void CGroupHTML::addImage(const std::string &id, const std::string &img, bool reloadImg, const CStyleParams &style)
	{
		// In a paragraph ?
		if (!_Paragraph)
		{
			newParagraph (0);
			paragraphChange ();
		}

		string finalUrl;

		// No more text in this text view
		_CurrentViewLink = NULL;

		// Not added ?
		CViewBitmap *newImage = new CViewBitmap (TCtorParam());
		newImage->setId(id);

		//
		// 1/ try to load the image with the old system (local files in bnp)
		//
		string image = CFile::getPath(img) + CFile::getFilenameWithoutExtension(img) + ".tga";
		if (lookupLocalFile (finalUrl, image.c_str(), false))
		{
			newImage->setRenderLayer(getRenderLayer()+1);
			image = finalUrl;
		}
		else
		{
			//
			// 2/ if it doesn't work, try to load the image in cache
			//
			image = localImageName(img);

			if (reloadImg && CFile::fileExists(image))
				CFile::deleteFile(image);

			if (lookupLocalFile (finalUrl, image.c_str(), false))
			{
				// don't display image that are not power of 2
				try
				{
					uint32 w, h;
					CBitmap::loadSize (image, w, h);
					if (w == 0 || h == 0 || ((!NLMISC::isPowerOf2(w) || !NLMISC::isPowerOf2(h)) && !NL3D::CTextureFile::supportNonPowerOfTwoTextures()))
						image = "web_del.tga";
				}
				catch(const NLMISC::Exception &e)
				{
					nlwarning(e.what());
					image = "web_del.tga";
				}
			}
			else
			{
				// no image in cache
				image = "web_del.tga";
			}

			addImageDownload(img, newImage, style);
		}
		newImage->setTexture (image);
		newImage->setModulateGlobalColor(style.GlobalColor);

		getParagraph()->addChild(newImage);
		paragraphChange ();

		setImageSize(newImage, style);
	}

	// ***************************************************************************

	CInterfaceGroup *CGroupHTML::addTextArea(const std::string &templateName, const char *name, uint rows, uint cols, bool multiLine, const ucstring &content, uint maxlength)
	{
		// In a paragraph ?
		if (!_Paragraph)
		{
			newParagraph (0);
			paragraphChange ();
		}

		// No more text in this text view
		_CurrentViewLink = NULL;

		CStyleParams &style = _Style.Current;
		{
			// override cols/rows values from style
			if (style.Width > 0) cols = style.Width / style.FontSize;
			if (style.Height > 0) rows = style.Height / style.FontSize;

			// Not added ?
			std::vector<std::pair<std::string,std::string> > templateParams;
			templateParams.push_back (std::pair<std::string,std::string> ("w", toString (cols*style.FontSize)));
			templateParams.push_back (std::pair<std::string,std::string> ("id", name));
			templateParams.push_back (std::pair<std::string,std::string> ("prompt", ""));
			templateParams.push_back (std::pair<std::string,std::string> ("multiline", multiLine?"true":"false"));
			templateParams.push_back (std::pair<std::string,std::string> ("fontsize", toString (style.FontSize)));
			templateParams.push_back (std::pair<std::string,std::string> ("color", style.TextColor.toString()));
			if (style.FontWeight >= FONT_WEIGHT_BOLD)
				templateParams.push_back (std::pair<std::string,std::string> ("fontweight", "bold"));
			if (style.FontOblique)
				templateParams.push_back (std::pair<std::string,std::string> ("fontstyle", "oblique"));
			if (multiLine)
				templateParams.push_back (std::pair<std::string,std::string> ("multi_min_line", toString(rows)));
			templateParams.push_back (std::pair<std::string,std::string> ("want_return", multiLine?"true":"false"));
			templateParams.push_back (std::pair<std::string,std::string> ("onenter", ""));
			templateParams.push_back (std::pair<std::string,std::string> ("enter_recover_focus", "false"));
			if (maxlength > 0)
				templateParams.push_back (std::pair<std::string,std::string> ("max_num_chars", toString(maxlength)));
			templateParams.push_back (std::pair<std::string,std::string> ("shadow", toString(style.TextShadow.Enabled)));
			if (style.TextShadow.Enabled)
			{
				templateParams.push_back (std::pair<std::string,std::string> ("shadow_x", toString(style.TextShadow.X)));
				templateParams.push_back (std::pair<std::string,std::string> ("shadow_y", toString(style.TextShadow.Y)));
				templateParams.push_back (std::pair<std::string,std::string> ("shadow_color", style.TextShadow.Color.toString()));
				templateParams.push_back (std::pair<std::string,std::string> ("shadow_outline", toString(style.TextShadow.Outline)));
			}

			CInterfaceGroup *textArea = CWidgetManager::getInstance()->getParser()->createGroupInstance (templateName.c_str(),
				getParagraph()->getId(), templateParams.empty()?NULL:&(templateParams[0]), (uint)templateParams.size());

			// Group created ?
			if (textArea)
			{
				// Set the content
				CGroupEditBox *eb = dynamic_cast<CGroupEditBox*>(textArea->getGroup("eb"));
				if (eb)
				{
					eb->setInputString(decodeHTMLEntities(content));
					if (style.hasStyle("background-color"))
					{
						CViewBitmap *bg = dynamic_cast<CViewBitmap*>(eb->getView("bg"));
						if (bg)
						{
							bg->setTexture("blank.tga");
							bg->setColor(style.BackgroundColor);
						}
					}
				}

				textArea->invalidateCoords();
				getParagraph()->addChild (textArea);
				paragraphChange ();

				return textArea;
			}
		}

		// Not group created
		return NULL;
	}

	// ***************************************************************************
	CDBGroupComboBox *CGroupHTML::addComboBox(const std::string &templateName, const char *name)
	{
		// In a paragraph ?
		if (!_Paragraph)
		{
			newParagraph (0);
			paragraphChange ();
		}


		{
			// Not added ?
			std::vector<std::pair<std::string,std::string> > templateParams;
			templateParams.push_back (std::pair<std::string,std::string> ("id", name));
			CInterfaceGroup *group = CWidgetManager::getInstance()->getParser()->createGroupInstance (templateName.c_str(),
				getParagraph()->getId(), templateParams.empty()?NULL:&(templateParams[0]), (uint)templateParams.size());

			// Group created ?
			if (group)
			{
				// Set the content
				CDBGroupComboBox *cb = dynamic_cast<CDBGroupComboBox *>(group);
				if (!cb)
				{
					nlwarning("'%s' template has bad type, combo box expected", templateName.c_str());
					delete cb;
					return NULL;
				}
				else
				{
					getParagraph()->addChild (cb);
					paragraphChange ();
					return cb;
				}
			}
		}

		// Not group created
		return NULL;
	}

	// ***************************************************************************
	CGroupMenu *CGroupHTML::addSelectBox(const std::string &templateName, const char *name)
	{
		// In a paragraph ?
		if (!_Paragraph)
		{
			newParagraph (0);
			paragraphChange ();
		}

		// Not added ?
		std::vector<std::pair<std::string,std::string> > templateParams;
		templateParams.push_back(std::pair<std::string,std::string> ("id", name));
		CInterfaceGroup *group = CWidgetManager::getInstance()->getParser()->createGroupInstance(templateName.c_str(),
			getParagraph()->getId(), &(templateParams[0]), (uint)templateParams.size());

		// Group created ?
		if (group)
		{
			// Set the content
			CGroupMenu *sb = dynamic_cast<CGroupMenu *>(group);
			if (!sb)
			{
				nlwarning("'%s' template has bad type, CGroupMenu expected", templateName.c_str());
				delete sb;
				return NULL;
			}
			else
			{
				getParagraph()->addChild (sb);
				paragraphChange ();
				return sb;
			}
		}

		// No group created
		return NULL;
	}

	// ***************************************************************************

	CCtrlButton *CGroupHTML::addButton(CCtrlButton::EType type, const std::string &name, const std::string &normalBitmap, const std::string &pushedBitmap,
									  const std::string &overBitmap, const char *actionHandler, const char *actionHandlerParams,
									  const char *tooltip, const CStyleParams &style)
	{
		// In a paragraph ?
		if (!_Paragraph)
		{
			newParagraph (0);
			paragraphChange ();
		}

		// Add the ctrl button
		CCtrlButton *ctrlButton = new CCtrlButton(TCtorParam());
		if (!name.empty())
		{
			ctrlButton->setId(name);
		}

		// Load only tga files.. (conversion in dds filename is done in the lookup procedure)
		string normal = normalBitmap.empty()?"":CFile::getPath(normalBitmap) + CFile::getFilenameWithoutExtension(normalBitmap) + ".tga";

		// if the image doesn't exist on local, we check in the cache
	//	if(!CFile::fileExists(normal))
		if(!CPath::exists(normal))
		{
			// search in the compressed texture
			CViewRenderer &rVR = *CViewRenderer::getInstance();
			sint32 id = rVR.getTextureIdFromName(normal);
			if(id == -1)
			{
				normal = localImageName(normalBitmap);
				if(!CFile::fileExists(normal))
				{
					normal = "web_del.tga";
				}
				else
				{
					try
					{
						uint32 w, h;
						CBitmap::loadSize(normal, w, h);
						if (w == 0 || h == 0)
							normal = "web_del.tga";
					}
					catch(const NLMISC::Exception &e)
					{
						nlwarning(e.what());
						normal = "web_del.tga";
					}
				}

				addImageDownload(normalBitmap, ctrlButton, style);
			}
		}

		string pushed = pushedBitmap.empty()?"":CFile::getPath(pushedBitmap) + CFile::getFilenameWithoutExtension(pushedBitmap) + ".tga";
		// if the image doesn't exist on local, we check in the cache, don't download it because the "normal" will already setuped it
	//	if(!CFile::fileExists(pushed))
		if(!CPath::exists(pushed))
		{
			// search in the compressed texture
			CViewRenderer &rVR = *CViewRenderer::getInstance();
			sint32 id = rVR.getTextureIdFromName(pushed);
			if(id == -1)
			{
				pushed = localImageName(pushedBitmap);
			}
		}

		string over = overBitmap.empty()?"":CFile::getPath(overBitmap) + CFile::getFilenameWithoutExtension(overBitmap) + ".tga";
		// schedule mouseover bitmap for download if its different from normal
		if (!over.empty() && !CPath::exists(over))
		{
			if (overBitmap != normalBitmap)
			{
				over = localImageName(overBitmap);
				addImageDownload(overBitmap, ctrlButton, style, OverImage);
			}
		}

		ctrlButton->setType (type);
		if (!normal.empty())
			ctrlButton->setTexture (normal);
		if (!pushed.empty())
			ctrlButton->setTexturePushed (pushed);
		if (!over.empty())
			ctrlButton->setTextureOver (over);
		ctrlButton->setModulateGlobalColorAll (style.GlobalColor);
		ctrlButton->setActionOnLeftClick (actionHandler);
		ctrlButton->setParamsOnLeftClick (actionHandlerParams);

		// Translate the tooltip or display raw text (tooltip from webig)
		if (tooltip)
		{
			if (CI18N::hasTranslation(tooltip))
			{
				ctrlButton->setDefaultContextHelp(CI18N::get(tooltip));
				//ctrlButton->setOnContextHelp(CI18N::get(tooltip).toString());
			}
			else
			{
				ctrlButton->setDefaultContextHelp(ucstring::makeFromUtf8(tooltip));
				//ctrlButton->setOnContextHelp(string(tooltip));
			}

			ctrlButton->setInstantContextHelp(true);
			ctrlButton->setToolTipParent(TTMouse);
			ctrlButton->setToolTipParentPosRef(Hotspot_TTAuto);
			ctrlButton->setToolTipPosRef(Hotspot_TTAuto);
		}

		getParagraph()->addChild (ctrlButton);
		paragraphChange ();

		setImageSize(ctrlButton, style);

		return ctrlButton;
	}

	// ***************************************************************************

	void CGroupHTML::flushString()
	{
		_CurrentViewLink = NULL;
	}

	// ***************************************************************************

	void CGroupHTML::clearContext()
	{
		_CurrentHTMLElement = NULL;
		_CurrentHTMLNextSibling = NULL;
		_Paragraph = NULL;
		_PRE.clear();
		_Indent.clear();
		_LI = false;
		_UL.clear();
		_DL.clear();
		_A.clear();
		_Link.clear();
		_LinkTitle.clear();
		_Tables.clear();
		_Cells.clear();
		_TR.clear();
		_Forms.clear();
		_Groups.clear();
		_Anchors.clear();
		_AnchorName.clear();
		_CellParams.clear();
		_Title = false;
		_TextArea = false;
		_Object = false;
		_Localize = false;
		_ReadingHeadTag = false;
		_IgnoreHeadTag = false;
		_IgnoreBaseUrlTag = false;

		// reset style
		resetCssStyle();

		// TR

		paragraphChange ();

		// clear the pointer to the current image download since all the button are deleted
	#ifdef LOG_DL
		nlwarning("Clear pointers to %d curls", Curls.size());
	#endif
		for(uint i = 0; i < Curls.size(); i++)
		{
			Curls[i].imgs.clear();
		}

		// remove download that are still queued
		for (vector<CDataDownload>::iterator it=Curls.begin(); it<Curls.end(); )
		{
			if (it->data == NULL) {
	#ifdef LOG_DL
		nlwarning("Remove waiting curl download (%s)", it->url.c_str());
	#endif
				it = Curls.erase(it);
			} else {
				++it;
			}
		}
	}

	// ***************************************************************************

	ucchar CGroupHTML::getLastChar() const
	{
		if (_CurrentViewLink)
		{
			const ucstring &str = _CurrentViewLink->getText();
			if (!str.empty())
				return str[str.length()-1];
		}
		return 0;
	}

	// ***************************************************************************

	void CGroupHTML::paragraphChange ()
	{
		_CurrentViewLink = NULL;
		_CurrentViewImage = NULL;
		CGroupParagraph *paragraph = getParagraph();
		if (paragraph)
		{
			// Number of child in this paragraph
			uint numChild = paragraph->getNumChildren();
			if (numChild)
			{
				// Get the last child
				CViewBase *child = paragraph->getChild(numChild-1);

				// Is this a string view ?
				_CurrentViewLink = dynamic_cast<CViewLink*>(child);
				_CurrentViewImage = dynamic_cast<CViewBitmap*>(child);
			}
		}
	}

	// ***************************************************************************

	CInterfaceGroup *CGroupHTML::getCurrentGroup()
	{
		if (!_Cells.empty() && _Cells.back())
			return _Cells.back()->Group;
		else
			return _GroupListAdaptor;
	}

	// ***************************************************************************

	void CGroupHTML::addHtmlGroup (CInterfaceGroup *group, uint beginSpace)
	{
		if (!group)
			return;

		registerAnchor(group);

		if (!_DivName.empty())
		{
			group->setName(_DivName);
			_Groups.push_back(group);
		}

		group->setSizeRef(CInterfaceElement::width);

		// Compute begin space between paragraph and tables
		// * If first in group, no begin space

		// Pointer on the current paragraph (can be a table too)
		CGroupParagraph *p = dynamic_cast<CGroupParagraph*>(group);

		CInterfaceGroup *parentGroup = CGroupHTML::getCurrentGroup();
		const std::vector<CInterfaceGroup*> &groups = parentGroup->getGroups ();
		group->setParent(parentGroup);
		group->setParentSize(parentGroup);
		if (groups.empty())
		{
			group->setParentPos(parentGroup);
			group->setPosRef(Hotspot_TL);
			group->setParentPosRef(Hotspot_TL);
			beginSpace = 0;
		}
		else
		{
			// Last is a paragraph ?
			group->setParentPos(groups.back());
			group->setPosRef(Hotspot_TL);
			group->setParentPosRef(Hotspot_BL);
		}

		// Set the begin space
		if (p)
			p->setTopSpace(beginSpace);
		else
			group->setY(-(sint32)beginSpace);
		parentGroup->addGroup (group);
	}

	// ***************************************************************************

	void CGroupHTML::setTitle (const ucstring &title)
	{
		CInterfaceElement *parent = getParent();
		if (parent)
		{
			if ((parent = parent->getParent()))
			{
				CGroupContainer *container = dynamic_cast<CGroupContainer*>(parent);
				if (container)
				{
					container->setUCTitle (title);
				}
			}
		}
	}

	void CGroupHTML::setTitle(const std::string &title)
	{
		ucstring uctitle;
		uctitle.fromUtf8(title);

		_TitleString.clear();
		if(!_TitlePrefix.empty())
		{
			_TitleString = _TitlePrefix + " - ";
		}
		_TitleString += uctitle;

		setTitle(_TitleString);
	}
	
	std::string CGroupHTML::getTitle() const {
		return _TitleString.toUtf8(); 
	};

	// ***************************************************************************

	bool CGroupHTML::lookupLocalFile (string &result, const char *url, bool isUrl)
	{
		result = url;
		string tmp;

		if (toLower(result).find("file:") == 0 && result.size() > 5)
		{
			result = result.substr(5, result.size()-5);
		}
		else if (result.find("://") != string::npos || result.find("//") == 0)
		{
			// http://, https://, etc or protocol-less url "//domain.com/image.png"
			return false;
		}

		tmp = CPath::lookup (CFile::getFilename(result), false, false, false);
		if (tmp.empty())
		{
			// try to find in local directory
			tmp = CPath::lookup (result, false, false, true);
		}

		if (!tmp.empty())
		{
			// Normalize the path
			if (isUrl)
				//result = "file:"+toLower(CPath::standardizePath (CPath::getFullPath (CFile::getPath(result)))+CFile::getFilename(result));*/
				result = "file:/"+tmp;
			else
				result = tmp;
			return true;
		}
		else
		{
			// Is it a texture in the big texture ?
			if (CViewRenderer::getInstance()->getTextureIdFromName (result) >= 0)
			{
				return true;
			}
			else
			{
				// This is not a file in the CPath, let libwww open this URL
				result = url;
				return false;
			}
		}
	}

	// ***************************************************************************

	void CGroupHTML::submitForm (uint formId, const char *submitButtonType, const char *submitButtonName, const char *submitButtonValue, sint32 x, sint32 y)
	{
		// Form id valid ?
		if (formId < _Forms.size())
		{
			_PostNextTime = true;
			_PostFormId = formId;
			_PostFormSubmitType = submitButtonType;
			_PostFormSubmitButton = submitButtonName;
			_PostFormSubmitValue = submitButtonValue;
			_PostFormSubmitX = x;
			_PostFormSubmitY = y;
		}
	}

	// ***************************************************************************

	void CGroupHTML::setBackgroundColor (const CRGBA &bgcolor)
	{
		// Should have a child named bg
		CViewBase *view = getView (DefaultBackgroundBitmapView);
		if (view)
		{
			CViewBitmap *bitmap = dynamic_cast<CViewBitmap*> (view);
			if (bitmap)
			{
				// Change the background color
				bitmap->setColor (bgcolor);
				bitmap->setModulateGlobalColor(false);
			}
		}
	}

	// ***************************************************************************

	void CGroupHTML::setBackground (const string &bgtex, bool scale, bool tile)
	{
		// Should have a child named bg
		CViewBase *view = getView (DefaultBackgroundBitmapView);
		if (view)
		{
			CViewBitmap *bitmap = dynamic_cast<CViewBitmap*> (view);
			if (bitmap)
			{
				bitmap->setParentPosRef(Hotspot_TL);
				bitmap->setPosRef(Hotspot_TL);
				bitmap->setX(0);
				bitmap->setY(0);
				bitmap->setRenderLayer(-2);
				bitmap->setScale(scale);
				bitmap->setTile(tile);
				addImageDownload(bgtex, view);
			}
		}
	}


	struct CButtonFreezer : public CInterfaceElementVisitor
	{
		virtual void visitCtrl(CCtrlBase *ctrl)
		{
			CCtrlBaseButton		*textButt = dynamic_cast<CCtrlTextButton *>(ctrl);
			if (textButt)
			{
				textButt->setFrozen(true);
			}
		}
	};

	// ***************************************************************************

	void CGroupHTML::handle ()
	{
		H_AUTO(RZ_Interface_Html_handle)

		const CWidgetManager::SInterfaceTimes &times = CWidgetManager::getInstance()->getInterfaceTimes();

		// handle curl downloads
		checkDownloads();

		// handle refresh timer
		if (_NextRefreshTime > 0 && _NextRefreshTime <= (times.thisFrameMs / 1000.0f) )
		{
			// there might be valid uses for 0sec refresh, but two in a row is probably a mistake
			if (_NextRefreshTime - _LastRefreshTime >= 1.0)
			{
				_LastRefreshTime = _NextRefreshTime;
				doBrowse(_RefreshUrl.c_str());
			}
			else
				nlwarning("Ignore second 0sec http-equiv refresh in a row (url '%s')", _URL.c_str());

			_NextRefreshTime = 0;
		}

		if (_Connecting)
		{
			// Check timeout if needed
			if (_TimeoutValue != 0 && _ConnectingTimeout <= ( times.thisFrameMs / 1000.0f ) )
			{
				browseError(("Connection timeout : "+_URL).c_str());

				_Connecting = false;
			}
		}
		else
		if (_RenderNextTime)
		{
			_RenderNextTime = false;
			renderHtmlString(_DocumentHtml);
		}
		else
		if (_WaitingForStylesheet)
		{
			renderDocument();
		}
		else
		if (_BrowseNextTime || _PostNextTime)
		{
			// Set timeout
			_Connecting = true;
			_ConnectingTimeout = ( times.thisFrameMs / 1000.0f ) + _TimeoutValue;

			// freeze form buttons
			CButtonFreezer freezer;
			this->visit(&freezer);

			// Home ?
			if (_URL == "home")
				_URL = home();

			string finalUrl;
			bool isLocal = lookupLocalFile (finalUrl, _URL.c_str(), true);

			_URL = finalUrl;

			CUrlParser uri (_URL);
			_TrustedDomain = isTrustedDomain(uri.host);
			_DocumentDomain = uri.host;

			// file is probably from bnp (ingame help)
			if (isLocal)
			{
				doBrowseLocalFile(finalUrl);
			}
			else
			{
				SFormFields formfields;
				if (_PostNextTime)
				{
					buildHTTPPostParams(formfields);
					// _URL is set from form.Action
					finalUrl = _URL;
				}
				else
				{
					// Add custom get params from child classes
					addHTTPGetParams (finalUrl, _TrustedDomain);
				}

				doBrowseRemoteUrl(finalUrl, "", _PostNextTime, formfields);
			}

			_BrowseNextTime = false;
			_PostNextTime = false;
		}
	}

	// ***************************************************************************
	void CGroupHTML::buildHTTPPostParams (SFormFields &formfields)
	{
		// Add text area text
		uint i;

		if (_PostFormId >= _Forms.size())
		{
			nlwarning("(%s) invalid form index %d, _Forms %d", _Id.c_str(), _PostFormId, _Forms.size());
			return;
		}
		// Ref the form
		CForm &form = _Forms[_PostFormId];

		_URL = form.Action;

		CUrlParser uri(_URL);
		_TrustedDomain = isTrustedDomain(uri.host);
		_DocumentDomain = uri.host;

		for (i=0; i<form.Entries.size(); i++)
		{
			// Text area ?
			bool addEntry = false;
			ucstring entryData;
			if (form.Entries[i].TextArea)
			{
				// Get the edit box view
				CInterfaceGroup *group = form.Entries[i].TextArea->getGroup ("eb");
				if (group)
				{
					// Should be a CGroupEditBox
					CGroupEditBox *editBox = dynamic_cast<CGroupEditBox*>(group);
					if (editBox)
					{
						entryData = editBox->getViewText()->getText();
						addEntry = true;
					}
				}
			}
			else if (form.Entries[i].Checkbox)
			{
				// todo handle unicode POST here
				if (form.Entries[i].Checkbox->getPushed ())
				{
                                        entryData = form.Entries[i].Value;
					addEntry = true;
				}
			}
			else if (form.Entries[i].ComboBox)
			{
				CDBGroupComboBox *cb = form.Entries[i].ComboBox;
				entryData.fromUtf8(form.Entries[i].SelectValues[cb->getSelection()]);
				addEntry = true;
			}
			else if (form.Entries[i].SelectBox)
			{
				CGroupMenu *sb = form.Entries[i].SelectBox;
				CGroupSubMenu *rootMenu = sb->getRootMenu();
				if (rootMenu)
				{
					for(uint j=0; j<rootMenu->getNumLine(); ++j)
					{
						CInterfaceGroup *ig = rootMenu->getUserGroupLeft(j);
						if (ig)
						{
							CCtrlBaseButton *cb = dynamic_cast<CCtrlBaseButton *>(ig->getCtrl("b"));
							if (cb && cb->getPushed())
								formfields.add(form.Entries[i].Name, form.Entries[i].SelectValues[j]);
						}
					}
				}
			}
			// This is a hidden value
			else
			{
				entryData = form.Entries[i].Value;
				addEntry = true;
			}

			// Add this entry
			if (addEntry)
			{
				formfields.add(form.Entries[i].Name, CI18N::encodeUTF8(entryData));
			}
		}

		if (_PostFormSubmitType == "image")
		{
			// Add the button coordinates
			if (_PostFormSubmitButton.find_first_of("[") == string::npos)
			{
				formfields.add(_PostFormSubmitButton + "_x", NLMISC::toString(_PostFormSubmitX));
				formfields.add(_PostFormSubmitButton + "_y", NLMISC::toString(_PostFormSubmitY));
			}
			else
			{
				formfields.add(_PostFormSubmitButton, NLMISC::toString(_PostFormSubmitX));
				formfields.add(_PostFormSubmitButton, NLMISC::toString(_PostFormSubmitY));
			}
		}
		else
			formfields.add(_PostFormSubmitButton, _PostFormSubmitValue);

		// Add custom params from child classes
		addHTTPPostParams(formfields, _TrustedDomain);
	}

	// ***************************************************************************
	void CGroupHTML::doBrowseLocalFile(const std::string &uri)
	{
		std::string filename;
		if (toLower(uri).find("file:/") == 0)
		{
			filename = uri.substr(6, uri.size() - 6);
		}
		else
		{
			filename = uri;
		}

	#if LOG_DL
		nlwarning("browse local file '%s'", filename.c_str());
	#endif

		_TrustedDomain = true;
		_DocumentDomain = "localhost";

		// Stop previous browse, remove content
		stopBrowse ();

		_Browsing = true;
		updateRefreshButton();

		CIFile in;
		if (in.open(filename))
		{
			std::string html;
			while(!in.eof())
			{
				char buf[1024];
				in.getline(buf, 1024);
				html += std::string(buf) + "\n";
			}
			in.close();

			if (!renderHtmlString(html))
			{
				browseError((string("Failed to parse html from file : ")+filename).c_str());
			}
		}
		else
		{
			browseError((string("The page address is malformed : ")+filename).c_str());
		}
	}

	// ***************************************************************************
	void CGroupHTML::doBrowseRemoteUrl(std::string url, const std::string &referer, bool doPost, const SFormFields &formfields)
	{
		// Stop previous request and remove content
		stopBrowse ();

		_Browsing = true;
		updateRefreshButton();

		// Reset the title
		if(_TitlePrefix.empty())
			setTitle (CI18N::get("uiPleaseWait"));
		else
			setTitle (_TitlePrefix + " - " + CI18N::get("uiPleaseWait"));

		url = upgradeInsecureUrl(url);

	#if LOG_DL
		nlwarning("(%s) browse url (trusted=%s) '%s', referer='%s', post='%s', nb form values %d",
				_Id.c_str(), (_TrustedDomain ? "true" :"false"), url.c_str(), referer.c_str(), (doPost ? "true" : "false"), formfields.Values.size());
	#endif

		if (!MultiCurl)
		{
			browseError(string("Invalid MultCurl handle, loading url failed : "+url).c_str());
			return;
		}

		CURL *curl = curl_easy_init();
		if (!curl)
		{
			nlwarning("(%s) failed to create curl handle", _Id.c_str());
			browseError(string("Failed to create cURL handle : " + url).c_str());
			return;
		}

		// https://
		if (toLower(url.substr(0, 8)) == "https://")
		{
			// if supported, use custom SSL context function to load certificates
			CCurlCertificates::useCertificates(curl);
		}

		// do not follow redirects, we have own handler
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0);
		// after redirect
		curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1);

		// tell curl to use compression if possible (gzip, deflate)
		// leaving this empty allows all encodings that curl supports
		//curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

		// limit curl to HTTP and HTTPS protocols only
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);

		// Destination
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

		// User-Agent:
		std::string userAgent = options.appName + "/" + options.appVersion;
		curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());

		// Cookies
		sendCookies(curl, _DocumentDomain, _TrustedDomain);

		// Referer
		if (!referer.empty())
		{
			curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
	#ifdef LOG_DL
			nlwarning("(%s) set referer '%s'", _Id.c_str(), referer.c_str());
	#endif
		}

		if (doPost)
		{
			// serialize form data and add it to curl
			std::string data;
			for(uint i=0; i<formfields.Values.size(); ++i)
			{
				char * escapedName = curl_easy_escape(curl, formfields.Values[i].name.c_str(), formfields.Values[i].name.size());
				char * escapedValue = curl_easy_escape(curl, formfields.Values[i].value.c_str(), formfields.Values[i].value.size());

				if (i>0)
					data += "&";

				data += std::string(escapedName) + "=" + escapedValue;

				curl_free(escapedName);
				curl_free(escapedValue);
			}
			curl_easy_setopt(curl, CURLOPT_POST, 1);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
			curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, data.c_str());
		}
		else
		{
			curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
		}

		// transfer handle
		_CurlWWW = new CCurlWWWData(curl, url);

		// set the language code used by the client
		std::vector<std::string> headers;
		headers.push_back("Accept-Language: "+options.languageCode);
		headers.push_back("Accept-Charset: utf-8");
		_CurlWWW->sendHeaders(headers);

		// catch headers for redirect
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NLGUI::curlHeaderCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEHEADER, _CurlWWW);

		// catch body
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NLGUI::curlDataCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, _CurlWWW);

	#if LOG_DL
		// progress callback
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, NLGUI::curlProgressCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, _CurlWWW);
	#else
		// progress off
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
	#endif

		//
		curl_multi_add_handle(MultiCurl, curl);

		// start the transfer
		int NewRunningCurls = 0;
		curl_multi_perform(MultiCurl, &NewRunningCurls);
		RunningCurls++;
	}

	// ***************************************************************************
	void CGroupHTML::htmlDownloadFinished(const std::string &content, const std::string &type, long code)
	{
	#ifdef LOG_DL
		nlwarning("(%s) HTML download finished, content length %d, type '%s', code %d", _Id.c_str(), content.size(), type.c_str(), code);
	#endif

		// create <html> markup for image downloads
		if (type.find("image/") == 0 && !content.empty())
		{
			try
			{
				std::string dest = localImageName(_URL);
				COFile out;
				out.open(dest);
				out.serialBuffer((uint8 *)(content.c_str()), content.size());
				out.close();
	#ifdef LOG_DL
				nlwarning("(%s) image saved to '%s', url '%s'", _Id.c_str(), dest.c_str(), _URL.c_str());
	#endif
			}
			catch(...) { }

			// create html code with image url inside and do the request again
			renderHtmlString("<html><head><title>"+_URL+"</title></head><body><img src=\"" + _URL + "\"></body></html>");
		}
		else if (_TrustedDomain && type.find("text/lua") == 0)
		{
			setTitle(_TitleString);

			_LuaScript = "\nlocal __CURRENT_WINDOW__=\""+this->_Id+"\" \n"+content;
			CLuaManager::getInstance().executeLuaScript(_LuaScript, true);
			_LuaScript.clear();
			
			_Browsing = false;
			_Connecting = false;

			// disable refresh button
			clearRefresh();
			// disable redo into this url
			_AskedUrl.clear();
		}
		else
		{
			renderHtmlString(content);
		}
	}

	// ***************************************************************************
	void CGroupHTML::cssDownloadFinished(const std::string &url, const std::string &local)
	{
		// remove from queue
		std::vector<std::string>::iterator it = std::find(_StylesheetQueue.begin(), _StylesheetQueue.end(), url);
		if (it != _StylesheetQueue.end())
		{
			_StylesheetQueue.erase(it);
		}

		if (!CFile::fileExists(local))
		{
			return;
		}

		parseStylesheetFile(local);
	}

	void CGroupHTML::renderDocument()
	{
		if (!_StylesheetQueue.empty())
		{
			// waiting for stylesheets to finish downloading
			return;
		}

		//TGameTime renderStart = CTime::getLocalTime();

		_Browsing = true;
		_WaitingForStylesheet = false;

		// keeps track of currently rendered element
		_CurrentHTMLElement = NULL;
		std::list<CHtmlElement>::iterator it = _HtmlDOM.Children.begin();
		while(it != _HtmlDOM.Children.end())
		{
			renderDOM(*it);
			++it;
		}
		_CurrentHTMLElement = NULL;

		// invalidate coords
		endBuild();

		// set the browser as complete
		_Browsing = false;
		updateRefreshButton();

		// check that the title is set, or reset it (in the case the page
		// does not provide a title)
		if (_TitleString.empty())
		{
			setTitle(_TitlePrefix);
		}

		//TGameTime renderStop = CTime::getLocalTime();
		//nlwarning("[%s] render: %.1fms (%s)\n", _Id.c_str(), (renderStop - renderStart), _URL.c_str());
	}

	// ***************************************************************************

	bool CGroupHTML::renderHtmlString(const std::string &html)
	{
		bool success;

		// if we are already rendering, then queue up the next page
		if (_CurrentHTMLElement)
		{
			_DocumentHtml = html;
			_RenderNextTime = true;

			return true;
		}

		//
		_Browsing = true;
		_DocumentUrl = _URL;
		_DocumentHtml = html;
		_NextRefreshTime = 0;
		_RefreshUrl.clear();

		// clear content
		beginBuild();
		resetCssStyle();

		// start new rendering
		_HtmlDOM = CHtmlElement(CHtmlElement::NONE, "<root>");

		if (!trim(html).empty())
		{
			success = parseHtml(html);
			if (success)
			{
				_WaitingForStylesheet = !_StylesheetQueue.empty();
				renderDocument();
			}
			else
			{
				std::string error = "ERROR: HTML parse failed.";
				error += toString("\nsize %d bytes", html.size());
				error += toString("\n---start---\n%s\n---end---\n", html.c_str());
				browseError(error.c_str());
			}
		}

		endBuild();
		updateRefreshButton();
		_Browsing = false;

		return success;
	}

	// ***************************************************************************
	void CGroupHTML::doBrowseAnchor(const std::string &anchor)
	{
		if (_Anchors.count(anchor) == 0)
		{
			return;
		}

		CInterfaceElement *pIE = _Anchors.find(anchor)->second;
		if (pIE)
		{
			// hotspot depends on vertical/horizontal scrollbar
			CCtrlScroll *pSB = getScrollBar();
			if (pSB)
			{
				pSB->ensureVisible(pIE, Hotspot_Tx, Hotspot_Tx);
			}
		}
	}

	// ***************************************************************************

	void CGroupHTML::draw ()
	{
		CGroupScrollText::draw ();
	}

	// ***************************************************************************

	void CGroupHTML::endBuild ()
	{
		invalidateCoords();
	}

	// ***************************************************************************

	void CGroupHTML::addHTTPGetParams (string &/* url */, bool /*trustedDomain*/)
	{
	}

	// ***************************************************************************

	void CGroupHTML::addHTTPPostParams (SFormFields &/* formfields */, bool /*trustedDomain*/)
	{
	}

	// ***************************************************************************
	void CGroupHTML::requestTerminated()
	{
		if (_CurlWWW)
		{
	#if LOG_DL
			nlwarning("(%s) stop curl, url '%s'", _Id.c_str(), _CurlWWW->Url.c_str());
	#endif
			if (MultiCurl)
				curl_multi_remove_handle(MultiCurl, _CurlWWW->Request);

			delete _CurlWWW;

			_CurlWWW = NULL;
			_Connecting = false;
		}
	}

	// ***************************************************************************

	string	CGroupHTML::home ()
	{
		return Home;
	}

	// ***************************************************************************

	void CGroupHTML::removeContent ()
	{
		// Remove old document
		if (!_GroupListAdaptor)
		{
			_GroupListAdaptor = new CGroupListAdaptor(CViewBase::TCtorParam()); // deleted by the list
			_GroupListAdaptor->setResizeFromChildH(true);
			getList()->addChild (_GroupListAdaptor, true);
		}

		// Group list adaptor not exist ?
		_GroupListAdaptor->clearGroups();
		_GroupListAdaptor->clearControls();
		_GroupListAdaptor->clearViews();
		CWidgetManager::getInstance()->clearViewUnders();
		CWidgetManager::getInstance()->clearCtrlsUnders();

		// Clear all the context
		clearContext();

		// Reset default background color
		setBackgroundColor (BgColor);

		paragraphChange ();
	}

	// ***************************************************************************
	const std::string &CGroupHTML::selectTreeNodeRecurs(CGroupTree::SNode *node, const std::string &url)
	{
		static std::string	emptyString;
		if(!node)
		{
			return emptyString;
		}

		// if this node match
		if(actionLaunchUrlRecurs(node->AHName, node->AHParams, url))
		{
			return node->Id;
		}
		// fails => look into children
		else
		{
			for(uint i=0;i<node->Children.size();i++)
			{
				const string &childRes= selectTreeNodeRecurs(node->Children[i], url);
				if(!childRes.empty())
					return childRes;
			}

			// none match...
			return emptyString;
		}
	}

	// ***************************************************************************
	bool	CGroupHTML::actionLaunchUrlRecurs(const std::string &ah, const std::string &params, const std::string &url)
	{
		// check if this action match
		if( (ah=="launch_help" || ah=="browse") && IActionHandler::getParam (params, "url") == url)
		{
			return true;
		}
		// can be a proc that contains launch_help/browse => look recurs
		else if(ah=="proc")
		{
			const std::string &procName= params;
			// look into this proc
			uint	numActions= CWidgetManager::getInstance()->getParser()->getProcedureNumActions(procName);
			for(uint i=0;i<numActions;i++)
			{
				string	procAh, procParams;
				if( CWidgetManager::getInstance()->getParser()->getProcedureAction(procName, i, procAh, procParams))
				{
					// recurs proc if needed!
					if (actionLaunchUrlRecurs(procAh, procParams, url))
						return true;
				}
			}
		}

		return false;
	}

	// ***************************************************************************
	void	CGroupHTML::clearRefresh()
	{
		_URL.clear();
		updateRefreshButton();
	}

	// ***************************************************************************
	void	CGroupHTML::clearUndoRedo()
	{
		// erase any undo/redo
		_BrowseUndo.clear();
		_BrowseRedo.clear();

		// update buttons validation
		updateUndoRedoButtons();
	}

	// ***************************************************************************
	void	CGroupHTML::pushUrlUndoRedo(const std::string &url)
	{
		// if same url, no op
		if(url==_AskedUrl)
			return;

		// erase any redo, push undo, set current
		_BrowseRedo.clear();
		if(!_AskedUrl.empty())
			_BrowseUndo.push_back(_AskedUrl);
		_AskedUrl= url;

		// limit undo
		while(_BrowseUndo.size()>MaxUrlUndoRedo)
			_BrowseUndo.pop_front();

		// update buttons validation
		updateUndoRedoButtons();
	}

	// ***************************************************************************
	void	CGroupHTML::browseUndo()
	{
		if(_BrowseUndo.empty())
			return;

		// push to redo, pop undo, and set current
		if (!_AskedUrl.empty())
			_BrowseRedo.push_front(_AskedUrl);

		_AskedUrl= _BrowseUndo.back();
		_BrowseUndo.pop_back();

		// update buttons validation
		updateUndoRedoButtons();

		// and then browse the undoed url, with no undo/redo
		doBrowse(_AskedUrl.c_str());
	}

	// ***************************************************************************
	void	CGroupHTML::browseRedo()
	{
		if(_BrowseRedo.empty())
			return;

		// push to undo, pop redo, and set current
		_BrowseUndo.push_back(_AskedUrl);
		_AskedUrl= _BrowseRedo.front();
		_BrowseRedo.pop_front();

		// update buttons validation
		updateUndoRedoButtons();

		// and then browse the redoed url, with no undo/redo
		doBrowse(_AskedUrl.c_str());
	}

	// ***************************************************************************
	void	CGroupHTML::updateUndoRedoButtons()
	{
		CCtrlBaseButton		*butUndo= dynamic_cast<CCtrlBaseButton *>(CWidgetManager::getInstance()->getElementFromId(_BrowseUndoButton));
		CCtrlBaseButton		*butRedo= dynamic_cast<CCtrlBaseButton *>(CWidgetManager::getInstance()->getElementFromId(_BrowseRedoButton));

		// gray according to list size
		if(butUndo)
			butUndo->setFrozen(_BrowseUndo.empty());
		if(butRedo)
			butRedo->setFrozen(_BrowseRedo.empty());
	}

	// ***************************************************************************
	void	CGroupHTML::updateRefreshButton()
	{
		CCtrlBaseButton		*butRefresh = dynamic_cast<CCtrlBaseButton *>(CWidgetManager::getInstance()->getElementFromId(_BrowseRefreshButton));

		bool enabled = !_Browsing && !_Connecting && !_URL.empty();
		if(butRefresh)
			butRefresh->setFrozen(!enabled);
	}

	// ***************************************************************************

	NLMISC_REGISTER_OBJECT(CViewBase, CGroupHTMLInputOffset, std::string, "html_input_offset");

	CGroupHTMLInputOffset::CGroupHTMLInputOffset(const TCtorParam &param)
		: CInterfaceGroup(param),
		Offset(0)
	{
	}

	xmlNodePtr CGroupHTMLInputOffset::serialize( xmlNodePtr parentNode, const char *type ) const
	{
		xmlNodePtr node = CInterfaceGroup::serialize( parentNode, type );
		if( node == NULL )
			return NULL;

		xmlSetProp( node, BAD_CAST "type", BAD_CAST "html_input_offset" );
		xmlSetProp( node, BAD_CAST "y_offset", BAD_CAST toString( Offset ).c_str() );

		return node;
	}

	// ***************************************************************************
	bool CGroupHTMLInputOffset::parse(xmlNodePtr cur, CInterfaceGroup *parentGroup)
	{
		if (!CInterfaceGroup::parse(cur, parentGroup)) return false;
		CXMLAutoPtr ptr;
		// Get the url
		ptr = xmlGetProp (cur, (xmlChar*)"y_offset");
		if (ptr)
			fromString((const char*)ptr, Offset);
		return true;
	}

	// ***************************************************************************
	int CGroupHTML::luaParseHtml(CLuaState &ls)
	{
		const char *funcName = "parseHtml";
		CLuaIHM::checkArgCount(ls, funcName, 1);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		std::string html = ls.toString(1);

		parseHtml(html);

		return 0;
	}

	int CGroupHTML::luaClearRefresh(CLuaState &ls)
	{
		const char *funcName = "clearRefresh";
		CLuaIHM::checkArgCount(ls, funcName, 0);

		clearRefresh();

		return 0;
	}

	int CGroupHTML::luaClearUndoRedo(CLuaState &ls)
	{
		const char *funcName = "clearUndoRedo";
		CLuaIHM::checkArgCount(ls, funcName, 0);

		clearUndoRedo();
		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaBrowse(CLuaState &ls)
	{
		const char *funcName = "browse";
		CLuaIHM::checkArgCount(ls, funcName, 1);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		browse(ls.toString(1));
		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaRefresh(CLuaState &ls)
	{
		const char *funcName = "refresh";
		CLuaIHM::checkArgCount(ls, funcName, 0);
		refresh();
		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaRemoveContent(CLuaState &ls)
	{
		const char *funcName = "removeContent";
		CLuaIHM::checkArgCount(ls, funcName, 0);
		removeContent();
		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaRenderHtml(CLuaState &ls)
	{
		const char *funcName = "renderHtml";
		CLuaIHM::checkArgCount(ls, funcName, 1);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		std::string html = ls.toString(1);

		// Always trust domain if rendered from lua
		_TrustedDomain = true;
		renderHtmlString(html);

		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaInsertText(CLuaState &ls)	
	{
		const char *funcName = "insertText";
		CLuaIHM::checkArgCount(ls, funcName, 3);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		CLuaIHM::checkArgType(ls, funcName, 2, LUA_TSTRING);
		CLuaIHM::checkArgType(ls, funcName, 3, LUA_TBOOLEAN);
		
		string name = ls.toString(1);

		ucstring text;
		text.fromUtf8(ls.toString(2));

		if (!_Forms.empty())
		{
			for (uint i=0; i<_Forms.back().Entries.size(); i++)
			{
				if (_Forms.back().Entries[i].TextArea && _Forms.back().Entries[i].Name == name)
				{
					// Get the edit box view
					CInterfaceGroup *group = _Forms.back().Entries[i].TextArea->getGroup ("eb");
					if (group)
					{
						// Should be a CGroupEditBox
						CGroupEditBox *editBox = dynamic_cast<CGroupEditBox*>(group);
						if (editBox)
							editBox->writeString(text, false, ls.toBoolean(3));
					}
				}
			}
		}

		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaAddString(CLuaState &ls)
	{
		const char *funcName = "addString";
		CLuaIHM::checkArgCount(ls, funcName, 1);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		addString(ucstring(ls.toString(1)));
		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaAddImage(CLuaState &ls)
	{
		const char *funcName = "addImage";
		CLuaIHM::checkArgCount(ls, funcName, 2);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		CLuaIHM::checkArgType(ls, funcName, 2, LUA_TBOOLEAN);
		if (!_Paragraph)
		{
			newParagraph(0);
			paragraphChange();
		}

		CStyleParams style;
		style.GlobalColor = ls.toBoolean(2);

		string url = getLink();
		if (!url.empty())
		{
			string params = "name=" + getId() + "|url=" + getLink ();
			addButton(CCtrlButton::PushButton, "", ls.toString(1), ls.toString(1),
								"", "browse", params.c_str(), "", style);
		}
		else
		{
			addImage("", ls.toString(1), false, style);
		}


		return 0;
	}

	// ***************************************************************************
	int CGroupHTML::luaShowDiv(CLuaState &ls)
	{
		const char *funcName = "showDiv";
		CLuaIHM::checkArgCount(ls, funcName, 2);
		CLuaIHM::checkArgType(ls, funcName, 1, LUA_TSTRING);
		CLuaIHM::checkArgType(ls, funcName, 2, LUA_TBOOLEAN);

		if (!_Groups.empty())
		{
			for (uint i=0; i<_Groups.size(); i++)
			{
				CInterfaceGroup *group = _Groups[i];
				if (group->getName() == ls.toString(1))
				{
					group->setActive(ls.toBoolean(2));
				}
			}
		}
		return 0;
	}

	// ***************************************************************************
	void CGroupHTML::setURL(const std::string &url)
	{
		browse(url.c_str());
	}

	// ***************************************************************************
	void CGroupHTML::parseStylesheetFile(const std::string &fname)
	{
		CIFile css;
		if (css.open(fname))
		{
			uint32 remaining = css.getFileSize();
			std::string content;
			try {
				while(!css.eof() && remaining > 0)
				{
					const uint BUF_SIZE = 4096;
					char buf[BUF_SIZE];

					uint32 readJustNow = std::min(remaining, BUF_SIZE);
					css.serialBuffer((uint8 *)&buf, readJustNow);
					content.append(buf, readJustNow);
					remaining -= readJustNow;
				}

				_Style.parseStylesheet(content);
			}
			catch(const Exception &e)
			{
				nlwarning("exception while reading css file '%s'", e.what());
			}
		}
		else
		{
			nlwarning("Stylesheet file '%s' not found (%s)", fname.c_str(), _URL.c_str());
		}
	}

	// ***************************************************************************
	bool CGroupHTML::parseHtml(const std::string &htmlString)
	{
		std::vector<std::string> links;
		std::string styleString;

		CHtmlElement *parsedDOM;
		if (_CurrentHTMLElement == NULL)
		{
			// parse under <root> element (clean dom)
			parsedDOM = &_HtmlDOM;
		}
		else
		{
			// parse under currently rendered <lua> element
			parsedDOM = _CurrentHTMLElement;
		}

		CHtmlParser parser;
		parser.getDOM(htmlString, *parsedDOM, styleString, links);

		if (!styleString.empty())
		{
			_Style.parseStylesheet(styleString);
		}
		if (!links.empty())
		{
			addStylesheetDownload(links);
		}

		// this should rarely fail as first element should be <html>
		bool success = parsedDOM->Children.size() > 0;

		std::list<CHtmlElement>::iterator it = parsedDOM->Children.begin();
		while(it != parsedDOM->Children.end())
		{
			if (it->Type == CHtmlElement::ELEMENT_NODE && it->Value == "html")
			{
				// more newly parsed childs from <body> into siblings
				if (_CurrentHTMLElement) {
					std::list<CHtmlElement>::iterator it2 = it->Children.begin();
					while(it2 != it->Children.end())
					{
						if (it2->Type == CHtmlElement::ELEMENT_NODE && it2->Value == "body")
						{
							spliceFragment(it2);
							break;
						}
						++it2;
					}
					// remove <html> fragment from current element child
					it = parsedDOM->Children.erase(it);
				}
				else
				{
					// remove link to <root> (html->parent == '<root>') or css selector matching will break
					it->parent = NULL;
					++it;
				}
				continue;
			}

			// skip over other non-handled element
			++it;
		}

		return success;
	}

	void CGroupHTML::spliceFragment(std::list<CHtmlElement>::iterator src)
	{
		if(!_CurrentHTMLElement->parent)
		{
			nlwarning("BUG: Current node is missing parent element. unable to splice fragment");
			return;
		}

		// get the iterators for current element (<lua>) and next sibling
		std::list<CHtmlElement>::iterator currentElement;
		currentElement = std::find(_CurrentHTMLElement->parent->Children.begin(), _CurrentHTMLElement->parent->Children.end(), *_CurrentHTMLElement);
		if (currentElement == _CurrentHTMLElement->parent->Children.end())
		{
			nlwarning("BUG: unable to find current element iterator from parent");
			return;
		}
		
		// where fragment should be moved
		std::list<CHtmlElement>::iterator insertBefore;
		if (_CurrentHTMLNextSibling == NULL)
		{
			insertBefore = _CurrentHTMLElement->parent->Children.end();
		} else {
			// get iterator for nextSibling
			insertBefore = std::find(_CurrentHTMLElement->parent->Children.begin(), _CurrentHTMLElement->parent->Children.end(), *_CurrentHTMLNextSibling);
		}

		_CurrentHTMLElement->parent->Children.splice(insertBefore, src->Children);

		// reindex moved elements
		CHtmlElement *prev = NULL;
		uint childIndex = _CurrentHTMLElement->childIndex;
		while(currentElement != _CurrentHTMLElement->parent->Children.end())
		{
			if (currentElement->Type == CHtmlElement::ELEMENT_NODE)
			{
				if (prev != NULL)
				{
					currentElement->parent = _CurrentHTMLElement->parent;
					currentElement->childIndex = childIndex;
					currentElement->previousSibling = prev;
					prev->nextSibling = &(*currentElement);
				}

				childIndex++;
				prev = &(*currentElement);
			}
			++currentElement;
		}
	}

	// ***************************************************************************
	inline bool isDigit(ucchar c, uint base = 16)
	{
		if (c>='0' && c<='9') return true;
		if (base != 16) return false;
		if (c>='A' && c<='F') return true;
		if (c>='a' && c<='f') return true;
		return false;
	}

	// ***************************************************************************
	inline ucchar convertHexDigit(ucchar c)
	{
		if (c>='0' && c<='9') return c-'0';
		if (c>='A' && c<='F') return c-'A'+10;
		if (c>='a' && c<='f') return c-'a'+10;
		return 0;
	}

	// ***************************************************************************
	ucstring CGroupHTML::decodeHTMLEntities(const ucstring &str)
	{
		ucstring result;
		uint last, pos;

		for (uint i=0; i<str.length(); ++i)
		{
			// HTML entity
			if (str[i] == '&' && (str.length()-i) >= 4)
			{
				pos = i+1;

				// unicode character
				if (str[pos] == '#')
				{
					++pos;

					// using decimal by default
					uint base = 10;

					// using hexadecimal if &#x
					if (str[pos] == 'x')
					{
						base = 16;
						++pos;
					}

					// setup "last" to point at the first character following "&#x?[0-9a-f]+"
					for (last = pos; last < str.length(); ++last) if (!isDigit(str[last], base)) break;

					// make sure that at least 1 digit was found
					// and have the terminating ';' to complete the token: "&#x?[0-9a-f]+;"
					if (last == pos || str[last] != ';')
					{
						result += str[i];
						continue;
					}

					ucchar c = 0;

					// convert digits to unicode character
					while (pos<last) c = convertHexDigit(str[pos++]) + c*ucchar(base);

					// append our new character to the result string
					result += c;

					// move 'i' forward to point at the ';' .. the for(...) will increment i to point to next char
					i = last;

					continue;
				}

				// special xml characters
				if (str.substr(i+1,5)==ucstring("quot;"))	{ i+=5; result+='\"'; continue; }
				if (str.substr(i+1,4)==ucstring("amp;"))	{ i+=4; result+='&'; continue; }
				if (str.substr(i+1,3)==ucstring("lt;"))	{ i+=3; result+='<'; continue; }
				if (str.substr(i+1,3)==ucstring("gt;"))	{ i+=3; result+='>'; continue; }
			}

			// all the special cases are catered for... treat this as a normal character
			result += str[i];
		}

		return result;
	}

	// ***************************************************************************
	std::string CGroupHTML::getAbsoluteUrl(const std::string &url)
	{
		CUrlParser uri(url);
		if (uri.isAbsolute())
			return url;

		uri.inherit(_URL);

		return uri.toString();
	}

	// ***************************************************************************
	void CGroupHTML::resetCssStyle()
	{
		_WaitingForStylesheet = false;
		_StylesheetQueue.clear();

		std::string css;

		// TODO: browser css
		css += "html { background-color: " + getRGBAString(BgColor) + "; color: " + getRGBAString(TextColor) + "; font-size: " + toString(TextFontSize) + "px;}";
		css += "a { color: " + getRGBAString(LinkColor) + "; text-decoration: underline; -ryzom-modulate-color: "+toString(LinkColorGlobalColor)+";}";
		css += "h1 { color: " + getRGBAString(H1Color) + "; font-size: "+ toString("%d", H1FontSize) + "px; -ryzom-modulate-color: "+toString(H1ColorGlobalColor)+";}";
		css += "h2 { color: " + getRGBAString(H2Color) + "; font-size: "+ toString("%d", H2FontSize) + "px; -ryzom-modulate-color: "+toString(H2ColorGlobalColor)+";}";
		css += "h3 { color: " + getRGBAString(H3Color) + "; font-size: "+ toString("%d", H3FontSize) + "px; -ryzom-modulate-color: "+toString(H3ColorGlobalColor)+";}";
		css += "h4 { color: " + getRGBAString(H4Color) + "; font-size: "+ toString("%d", H4FontSize) + "px; -ryzom-modulate-color: "+toString(H4ColorGlobalColor)+";}";
		css += "h5 { color: " + getRGBAString(H5Color) + "; font-size: "+ toString("%d", H5FontSize) + "px; -ryzom-modulate-color: "+toString(H5ColorGlobalColor)+";}";
		css += "h6 { color: " + getRGBAString(H6Color) + "; font-size: "+ toString("%d", H6FontSize) + "px; -ryzom-modulate-color: "+toString(H6ColorGlobalColor)+";}";
		css += "input[type=\"text\"] { color: " + getRGBAString(TextColor) + "; font-size: " + toString("%d", TextFontSize) + "px; font-weight: normal; text-shadow: 1px 1px #000;}";
		css += "pre { font-family: monospace;}";
		// th { text-align: center; } - overwrites align property
		css += "th { font-weight: bold; }";
		css += "textarea { color: " + getRGBAString(TextColor) + "; font-weight: normal; font-size: " + toString("%d", TextFontSize) + "px; text-shadow: 1px 1px #000;}";
		css += "del { text-decoration: line-through;}";
		css += "u { text-decoration: underline;}";
		css += "em { font-style: italic; }";
		css += "strong { font-weight: bold; }";
		css += "small { font-size: smaller;}";
		css += "dt { font-weight: bold; }";
		css += "hr { color: rgb(120, 120, 120);}";
		// td { padding: 1px;} - overwrites cellpadding attribute
		// table { border-spacing: 2px;} - overwrites cellspacing attribute
		css += "table { border-collapse: separate;}";

		_Style.reset();
		_Style.parseStylesheet(css);
	}
	
	// ***************************************************************************
	std::string CGroupHTML::HTMLOListElement::getListMarkerText() const
	{
		std::string ret;
		sint32 number = Value;

		if (Type == "disc")
		{
			// (ucchar)0x2219;
			ret = "\xe2\x88\x99 ";
		}
		else if (Type == "circle")
		{
			// (uchar)0x26AA;
			ret = "\xe2\x9a\xaa ";
		}
		else if (Type == "square")
		{
			// (ucchar)0x25AA;
			ret = "\xe2\x96\xaa ";
		}
		else if (Type == "a" || Type == "A")
		{
			// @see toAlphabeticOrNumeric in WebKit
			static const char lower[26] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
											'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z' };
			static const char upper[26] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
											'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' };
			uint size = 26;
			if (number < 1)
			{
				ret = toString(number);
			}
			else
			{
				const char* digits = (Type == "A" ? upper : lower);
				while(number > 0)
				{
					--number;
					ret.insert(ret.begin(), digits[number % size]);
					number /= size;
				}
			}
			ret += ". ";
		}
		else if (Type == "i" || Type == "I")
		{
			// @see toRoman in WebKit
			static const char lower[7] = {'i', 'v', 'x', 'l', 'c', 'd', 'm'};
			static const char upper[7] = {'I', 'V', 'X', 'L', 'C', 'D', 'M'};

			if (number < 1 || number > 3999)
			{
				ret = toString(number);
			}
			else
			{
				const char* digits = (Type == "I" ? upper : lower);
				uint8 i, d=0;
				do
				{
					uint32 num = number % 10;
					if (num % 5 < 4)
					{
						for (i = num % 5; i > 0; i--)
						{
							ret.insert(ret.begin(), digits[d]);
						}
					}
					if (num >= 4 && num <= 8)
					{
						ret.insert(ret.begin(), digits[d + 1]);
					}
					if (num == 9)
					{
						ret.insert(ret.begin(), digits[d + 2]);
					}
					if (num % 5 == 4)
					{
						ret.insert(ret.begin(), digits[d]);
					}
					number /= 10;
					d += 2;
				}
				while (number > 0);

				if (Type == "I")
				{
					ret = toUpper(ret);
				}
			}
			ret += ". ";
		}
		else
		{
			ret = toString(Value) + ". ";
		}

		return ret;
	}

	void CGroupHTML::getCellsParameters(const CHtmlElement &elm, bool inherit)
	{
		CGroupHTML::CCellParams cellParams;
		if (!_CellParams.empty() && inherit)
		{
			cellParams = _CellParams.back();
		}

		if (_Style.hasStyle("background-color"))
			cellParams.BgColor = _Style.Current.BackgroundColor;
		else if (elm.hasNonEmptyAttribute("bgcolor"))
			scanHTMLColor(elm.getAttribute("bgcolor").c_str(), cellParams.BgColor);

		if (elm.hasAttribute("nowrap") || _Style.Current.WhiteSpace == "nowrap")
			cellParams.NoWrap = true;

		if (elm.hasNonEmptyAttribute("l_margin"))
			fromString(elm.getAttribute("l_margin"), cellParams.LeftMargin);

		{
			std::string align;
			// having text-align on table/tr should not override td align attribute
			if (_Style.hasStyle("text-align"))
				align = _Style.Current.TextAlign;
			else if (elm.hasNonEmptyAttribute("align"))
				align = toLower(elm.getAttribute("align"));

			if (align == "left")
				cellParams.Align = CGroupCell::Left;
			else if (align == "center")
				cellParams.Align = CGroupCell::Center;
			else if (align == "right")
				cellParams.Align = CGroupCell::Right;
		}

		{
			std::string valign;
			if (_Style.hasStyle("vertical-align"))
				valign = _Style.Current.VerticalAlign;
			else if (elm.hasNonEmptyAttribute("valign"))
				valign = toLower(elm.getAttribute("valign"));
			
			if (valign == "top")
				cellParams.VAlign = CGroupCell::Top;
			else if (valign == "middle")
				cellParams.VAlign = CGroupCell::Middle;
			else if (valign == "bottom")
				cellParams.VAlign = CGroupCell::Bottom;
		}
		
		_CellParams.push_back (cellParams);
	}

	// ***************************************************************************
	void CGroupHTML::htmlA(const CHtmlElement &elm)
	{
		_A.push_back(true);
		_Link.push_back ("");
		_LinkTitle.push_back("");
		_LinkClass.push_back("");
		if (elm.hasClass("ryzom-ui-button"))
			_LinkClass.back() = "ryzom-ui-button";

		// #fragment works with both ID and NAME so register both
		if (elm.hasNonEmptyAttribute("name"))
			_AnchorName.push_back(elm.getAttribute("name"));
		if (elm.hasNonEmptyAttribute("title"))
			_LinkTitle.back() = elm.getAttribute("title");
		if (elm.hasNonEmptyAttribute("href"))
		{
			string suri = elm.getAttribute("href");
			if(suri.find("ah:") == 0)
			{
				if (_TrustedDomain)
					_Link.back() = suri;
			}
			else if (_TrustedDomain && suri[0] == '#' && _LuaHrefHack)
			{
				// Direct url (hack for lua beginElement)
				_Link.back() = suri.substr(1);
			}
			else
			{
				// convert href from "?key=val" into "http://domain.com/?key=val"
				_Link.back() = getAbsoluteUrl(suri);
			}
		}

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlAend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);

		popIfNotEmpty(_A);
		popIfNotEmpty(_Link);
		popIfNotEmpty(_LinkTitle);
		popIfNotEmpty(_LinkClass);
	}

	// ***************************************************************************
	void CGroupHTML::htmlBASE(const CHtmlElement &elm)
	{
		if (!_ReadingHeadTag || _IgnoreBaseUrlTag)
			return;

		if (elm.hasNonEmptyAttribute("href"))
		{
			CUrlParser uri(elm.getAttribute("href"));
			if (uri.isAbsolute())
			{
				_URL = uri.toString();
				_IgnoreBaseUrlTag = true;
			}
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlBODY(const CHtmlElement &elm)
	{
		// override <body> (or <html>) css style attribute
		if (elm.hasNonEmptyAttribute("bgcolor"))
		{
			_Style.applyStyle("background-color: " + elm.getAttribute("bgcolor"));
		}

		if (_Style.hasStyle("background-color"))
		{
			CRGBA bgColor = _Style.Current.BackgroundColor;
			scanHTMLColor(elm.getAttribute("bgcolor").c_str(), bgColor);
			setBackgroundColor(bgColor);
		}

		if (elm.hasNonEmptyAttribute("style"))
		{
			string style = elm.getAttribute("style");

			TStyle styles = parseStyle(style);
			TStyle::iterator	it;

			it = styles.find("background-repeat");
			bool repeat = (it != styles.end() && it->second == "1");

			// Webig only
			it = styles.find("background-scale");
			bool scale = (it != styles.end() && it->second == "1");

			it = styles.find("background-image");
			if (it != styles.end())
			{
				string image = it->second;
				string::size_type texExt = toLower(image).find("url(");
				// Url image
				if (texExt != string::npos)
					// Remove url()
					image = image.substr(4, image.size()-5);
				setBackground (image, scale, repeat);
			}
		}

		renderPseudoElement(":before", elm);
	}

	// ***************************************************************************
	void CGroupHTML::htmlBR(const CHtmlElement &elm)
	{
		endParagraph();

		// insert zero-width-space (0x200B) to prevent removal of empty lines
		ucstring tmp;
		tmp.fromUtf8("\xe2\x80\x8b");
		addString(tmp);
	}

	// ***************************************************************************
	void CGroupHTML::htmlDD(const CHtmlElement &elm)
	{
		if (_DL.empty())
			return;

		// if there was no closing tag for <dt>, then remove <dt> style
		if (_DL.back().DT)
		{
			nlwarning("BUG: nested DT in DD");
			_DL.back().DT = false;
		}

		if (_DL.back().DD)
		{
			nlwarning("BUG: nested DD in DD");
			_DL.back().DD = false;
			popIfNotEmpty(_Indent);
		}

		_DL.back().DD = true;
		_Indent.push_back(getIndent() + ULIndent);

		if (!_LI)
		{
			_LI = true;
			newParagraph(ULBeginSpace);
		}
		else
		{
			newParagraph(LIBeginSpace);
		}

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlDDend(const CHtmlElement &elm)
	{
		if (_DL.empty())
			return;

		renderPseudoElement(":after", elm);

		// parser will process two DD in a row as nested when first DD is not closed
		if (_DL.back().DD)
		{
			_DL.back().DD = false;
			popIfNotEmpty(_Indent);
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlDIV(const CHtmlElement &elm)
	{
		_BlockLevelElement.push_back(true);

		_DivName = elm.getAttribute("name");

		string instClass = elm.getAttribute("class");

		// use generic template system
		if (_TrustedDomain && !instClass.empty() && instClass == "ryzom-ui-grouptemplate")
		{
			string style = elm.getAttribute("style");
			string id = elm.getAttribute("id");

			typedef pair<string, string> TTmplParam;
			vector<TTmplParam> tmplParams;

			string templateName;
			if (!style.empty())
			{
				TStyle styles = parseStyle(style);
				TStyle::iterator	it;
				for (it=styles.begin(); it != styles.end(); it++)
				{
					if ((*it).first == "template")
						templateName = (*it).second;
					else if ((*it).first == "display" && (*it).second == "inline-block")
						_BlockLevelElement.back() = false;
					else
						tmplParams.push_back(TTmplParam((*it).first, (*it).second));
				}
			}

			if (!templateName.empty())
			{
				string parentId;
				bool haveParentDiv = getDiv() != NULL;
				if (haveParentDiv)
					parentId = getDiv()->getId();
				else
				{
					if (!_Paragraph)
						newParagraph (0);

					parentId = _Paragraph->getId();
				}

				CInterfaceGroup *inst = CWidgetManager::getInstance()->getParser()->createGroupInstance(templateName, this->_Id+":"+id, tmplParams);
				if (inst)
				{
					inst->setId(this->_Id+":"+id);
					inst->updateCoords();
					if (haveParentDiv)
					{
							inst->setParent(getDiv());
							inst->setParentSize(getDiv());
							inst->setParentPos(getDiv());
							inst->setPosRef(Hotspot_TL);
							inst->setParentPosRef(Hotspot_TL);
							getDiv()->addGroup(inst);

							_BlockLevelElement.back() = false;
					}
					else
					{
						getParagraph()->addChild(inst);
						paragraphChange();
					}
					_Divs.push_back(inst);
				}
			}
		}

		if (isBlockLevelElement())
		{
			newParagraph(0);
		}

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlDIVend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);

		if (isBlockLevelElement())
		{
			endParagraph();
		}
		_DivName.clear();
		popIfNotEmpty(_Divs);
		popIfNotEmpty(_BlockLevelElement);
	}

	// ***************************************************************************
	void CGroupHTML::htmlDL(const CHtmlElement &elm)
	{
		_DL.push_back(HTMLDListElement());
		_LI = _DL.size() > 1 || !_UL.empty();
		endParagraph();

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlDLend(const CHtmlElement &elm)
	{
		if (_DL.empty())
			return;

		renderPseudoElement(":after", elm);

		endParagraph();

		// unclosed DT
		if (_DL.back().DT)
		{
			nlwarning("BUG: unclosed DT in DL");
		}

		// unclosed DD
		if (_DL.back().DD)
		{
			popIfNotEmpty(_Indent);
			nlwarning("BUG: unclosed DD in DL");
		}

		popIfNotEmpty (_DL);
	}

	// ***************************************************************************
	void CGroupHTML::htmlDT(const CHtmlElement &elm)
	{
		if (_DL.empty())
			return;

		// TODO: check if nested tags still happen and fix it in parser
		//     : remove special handling for nesting and let it happen

		// html parser and libxml2 should prevent nested tags like these
		if (_DL.back().DD)
		{
			nlwarning("BUG: nested DD in DT");

			_DL.back().DD = false;
			popIfNotEmpty(_Indent);
		}

		// html parser and libxml2 should prevent nested tags like these
		if (_DL.back().DT)
		{
			nlwarning("BUG: nested DT in DT");
		}

		_DL.back().DT = true;

		if (!_LI)
		{
			_LI = true;
			newParagraph(ULBeginSpace);
		}
		else
		{
			newParagraph(LIBeginSpace);
		}
		
		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlDTend(const CHtmlElement &elm)
	{
		if (_DL.empty())
			return;

		renderPseudoElement(":after", elm);

		_DL.back().DT = false;
	}

	// ***************************************************************************
	void CGroupHTML::htmlFONT(const CHtmlElement &elm)
	{
		if (elm.hasNonEmptyAttribute("color"))
		{
			CRGBA color;
			if (scanHTMLColor(elm.getAttribute("color").c_str(), color))
				_Style.Current.TextColor = color;
		}

		if (elm.hasNonEmptyAttribute("size"))
		{
			uint fontsize;
			fromString(elm.getAttribute("size"), fontsize);
			_Style.Current.FontSize = fontsize;
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlFORM(const CHtmlElement &elm)
	{
		// Build the form
		CGroupHTML::CForm form;

		// Get the action name
		if (elm.hasNonEmptyAttribute("action"))
		{
			form.Action = getAbsoluteUrl(elm.getAttribute("action"));
		}
		else
		{
			form.Action = _URL;
		}

		_Forms.push_back(form);

		renderPseudoElement(":before", elm);
	}

	// ***************************************************************************
	void CGroupHTML::htmlH(const CHtmlElement &elm)
	{
		newParagraph(PBeginSpace);
		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlHend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);
		endParagraph();
	}

	// ***************************************************************************
	void CGroupHTML::htmlHEAD(const CHtmlElement &elm)
	{
		_ReadingHeadTag = !_IgnoreHeadTag;
		_IgnoreHeadTag = true;
	}

	void CGroupHTML::htmlHEADend(const CHtmlElement &elm)
	{
		_ReadingHeadTag = false;
	}

	// ***************************************************************************
	void CGroupHTML::htmlHR(const CHtmlElement &elm)
	{
		newParagraph(0);

		CInterfaceGroup *sep = CWidgetManager::getInstance()->getParser()->createGroupInstance("html_hr", "", NULL, 0);
		if (sep)
		{
			CViewBitmap *bitmap = dynamic_cast<CViewBitmap*>(sep->getView("hr"));
			if (bitmap)
			{
				bitmap->setColor(_Style.Current.TextColor);
				if (_Style.Current.Width > 0)
				{
					clamp(_Style.Current.Width, 1, 32000);
					bitmap->setW(_Style.Current.Width);
					bitmap->setSizeRef(CInterfaceElement::none);
				}
				if (_Style.Current.Height > 0)
				{
					clamp(_Style.Current.Height, 1, 1000);
					bitmap->setH(_Style.Current.Height);
				}
			}

			renderPseudoElement(":before", elm);
			getParagraph()->addChild(sep);
			renderPseudoElement(":after", elm);

			endParagraph();
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlHTML(const CHtmlElement &elm)
	{
		if (elm.hasNonEmptyAttribute("style"))
		{
			_Style.Root = _Style.Current;
			_Style.applyRootStyle(elm.getAttribute("style"));
			_Style.Current = _Style.Root;
		}
		setBackgroundColor(_Style.Current.BackgroundColor);
	}

	// ***************************************************************************
	void CGroupHTML::htmlI(const CHtmlElement &elm)
	{
		_Localize = true;
		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlIend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);
		_Localize = false;
	}

	// ***************************************************************************
	void CGroupHTML::htmlIMG(const CHtmlElement &elm)
	{
		// Get the string name
		if (elm.hasNonEmptyAttribute("src"))
		{
			float tmpf;
			std::string id = elm.getAttribute("id");
			std::string src = elm.getAttribute("src");

			if (elm.hasNonEmptyAttribute("width"))
				getPercentage(_Style.Current.Width, tmpf, elm.getAttribute("width").c_str());
			if (elm.hasNonEmptyAttribute("height"))
				getPercentage(_Style.Current.Height, tmpf, elm.getAttribute("height").c_str());

			// Get the global color name
			if (elm.hasAttribute("global_color"))
				_Style.Current.GlobalColor = true;

			// Tooltip
			// keep "alt" attribute for backward compatibility
			std::string strtooltip = elm.getAttribute("alt");
			// tooltip
			if (elm.hasNonEmptyAttribute("title"))
				strtooltip = elm.getAttribute("title");

			const char *tooltip = NULL;
			// note: uses pointer to string data
			if (!strtooltip.empty())
				tooltip = strtooltip.c_str();

			// Mouse over image
			string overSrc = elm.getAttribute("data-over-src");

			if (getA() && getParent () && getParent ()->getParent())
			{
				string params = "name=" + getId() + "|url=" + getLink ();
				addButton(CCtrlButton::PushButton, id, src, src, overSrc, "browse", params.c_str(), tooltip, _Style.Current);
			}
			else
			if (tooltip || !overSrc.empty())
			{
				addButton(CCtrlButton::PushButton, id, src, src, overSrc, "", "", tooltip, _Style.Current);
			}
			else
			{
				// Get the option to reload (class==reload)
				bool reloadImg = false;

				if (elm.hasNonEmptyAttribute("style"))
				{
					string styleString = elm.getAttribute("style");
					TStyle styles = parseStyle(styleString);
					TStyle::iterator	it;

					it = styles.find("reload");
					if (it != styles.end() && (*it).second == "1")
						reloadImg = true;
				}

				addImage(id, elm.getAttribute("src"), reloadImg, _Style.Current);
			}
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlINPUT(const CHtmlElement &elm)
	{
		if (_Forms.empty())
			return;

		// read general property
		string id = elm.getAttribute("id");

		// Widget template name (old)
		string templateName = elm.getAttribute("z_btn_tmpl");
		// Input name is the new
		if (elm.hasNonEmptyAttribute("z_input_tmpl"))
			templateName = elm.getAttribute("z_input_tmpl");

		// Widget minimal width
		string minWidth = elm.getAttribute("z_input_width");

		// Get the type
		if (elm.hasNonEmptyAttribute("type"))
		{
			// Global color flag
			if (elm.hasAttribute("global_color"))
				_Style.Current.GlobalColor = true;

			// Tooltip
			std::string strtooltip = elm.getAttribute("alt");
			const char *tooltip = NULL;
			// note: uses pointer to strtooltip data
			if (!strtooltip.empty())
				tooltip = strtooltip.c_str();

			string type = toLower(elm.getAttribute("type"));
			if (type == "image")
			{
				// The submit button
				string name = elm.getAttribute("name");
				string normal = elm.getAttribute("src");
				string pushed;
				string over;

				// Action handler parameters : "name=group_html_id|form=id_of_the_form|submit_button=button_name"
				string param = "name=" + getId() + "|form=" + toString (_Forms.size()-1) + "|submit_button=" + name + "|submit_button_type=image";

				// Add the ctrl button
				addButton (CCtrlButton::PushButton, name, normal, pushed.empty()?normal:pushed, over,
					"html_submit_form", param.c_str(), tooltip, _Style.Current);
			}
			else if (type == "button" || type == "submit")
			{
				// The submit button
				string name = elm.getAttribute("name");
				string normal = elm.getAttribute("src");
				string text = elm.getAttribute("value");
				string pushed;
				string over;

				string buttonTemplate(!templateName.empty() ? templateName : DefaultButtonGroup );

				// Action handler parameters : "name=group_html_id|form=id_of_the_form|submit_button=button_name"
				string param = "name=" + getId() + "|form=" + toString (_Forms.size()-1) + "|submit_button=" + name + "|submit_button_type=submit";
				if (!text.empty())
				{
					// escape AH param separator
					string tmp = text;
					while(NLMISC::strFindReplace(tmp, "|", "&#124;"))
						;
					param = param + "|submit_button_value=" + tmp;
				}

				// Add the ctrl button
				if (!_Paragraph)
				{
					newParagraph (0);
					paragraphChange ();
				}

				typedef pair<string, string> TTmplParam;
				vector<TTmplParam> tmplParams;
				tmplParams.push_back(TTmplParam("id", name));
				tmplParams.push_back(TTmplParam("onclick", "html_submit_form"));
				tmplParams.push_back(TTmplParam("onclick_param", param));
				//tmplParams.push_back(TTmplParam("text", text));
				tmplParams.push_back(TTmplParam("active", "true"));
				if (!minWidth.empty())
					tmplParams.push_back(TTmplParam("wmin", minWidth));
				CInterfaceGroup *buttonGroup = CWidgetManager::getInstance()->getParser()->createGroupInstance(buttonTemplate, _Paragraph->getId(), tmplParams);
				if (buttonGroup)
				{

					// Add the ctrl button
					CCtrlTextButton *ctrlButton = dynamic_cast<CCtrlTextButton*>(buttonGroup->getCtrl("button"));
					if (!ctrlButton) ctrlButton = dynamic_cast<CCtrlTextButton*>(buttonGroup->getCtrl("b"));
					if (ctrlButton)
					{
						ctrlButton->setModulateGlobalColorAll (_Style.Current.GlobalColor);

						// Translate the tooltip
						if (tooltip)
						{
							if (CI18N::hasTranslation(tooltip))
							{
								ctrlButton->setDefaultContextHelp(CI18N::get(tooltip));
							}
							else
							{
								ctrlButton->setDefaultContextHelp(ucstring(tooltip));
							}
						}

						ctrlButton->setText(ucstring::makeFromUtf8(text));

						setTextButtonStyle(ctrlButton, _Style.Current);
					}
					getParagraph()->addChild (buttonGroup);
					paragraphChange ();
				}
			}
			else if (type == "text")
			{
				// Get the string name
				string name = elm.getAttribute("name");
				ucstring ucValue;
				ucValue.fromUtf8(elm.getAttribute("value"));

				uint size = 120;
				uint maxlength = 1024;
				if (elm.hasNonEmptyAttribute("size"))
					fromString(elm.getAttribute("size"), size);
				if (elm.hasNonEmptyAttribute("maxlength"))
					fromString(elm.getAttribute("maxlength"), maxlength);

				string textTemplate(!templateName.empty() ? templateName : DefaultFormTextGroup);
				// Add the editbox
				CInterfaceGroup *textArea = addTextArea (textTemplate, name.c_str (), 1, size/12, false, ucValue, maxlength);
				if (textArea)
				{
					// Add the text area to the form
					CGroupHTML::CForm::CEntry entry;
					entry.Name = name;
					entry.TextArea = textArea;
					_Forms.back().Entries.push_back (entry);
				}
			}
			else if (type == "checkbox" || type == "radio")
			{
				renderPseudoElement(":before", elm);

				CCtrlButton::EType btnType;
				string name = elm.getAttribute("name");
				string normal = elm.getAttribute("src");
				string pushed;
				string over;
				ucstring ucValue = ucstring("on");
				bool checked = elm.hasAttribute("checked");

				// TODO: unknown if empty attribute should override or not
				if (elm.hasNonEmptyAttribute("value"))
					ucValue.fromUtf8(elm.getAttribute("value"));

				if (type == "radio")
				{
					btnType = CCtrlButton::RadioButton;
					normal = DefaultRadioButtonBitmapNormal;
					pushed = DefaultRadioButtonBitmapPushed;
					over = DefaultRadioButtonBitmapOver;
				}
				else
				{
					btnType = CCtrlButton::ToggleButton;
					normal = DefaultCheckBoxBitmapNormal;
					pushed = DefaultCheckBoxBitmapPushed;
					over = DefaultCheckBoxBitmapOver;
				}

				// Add the ctrl button
				CCtrlButton *checkbox = addButton (btnType, name, normal, pushed, over, "", "", tooltip, _Style.Current);
				if (checkbox)
				{
					if (btnType == CCtrlButton::RadioButton)
					{
						// override with 'id' because radio buttons share same name
						if (!id.empty())
							checkbox->setId(id);

						// group together buttons with same name
						CForm &form = _Forms.back();
						bool notfound = true;
						for (uint i=0; i<form.Entries.size(); i++)
						{
							if (form.Entries[i].Name == name && form.Entries[i].Checkbox->getType() == CCtrlButton::RadioButton)
							{
								checkbox->initRBRefFromRadioButton(form.Entries[i].Checkbox);
								notfound = false;
								break;
							}
						}
						if (notfound)
						{
							// this will start a new group (initRBRef() would take first button in group container otherwise)
							checkbox->initRBRefFromRadioButton(checkbox);
						}
					}

					checkbox->setPushed (checked);

					// Add the button to the form
					CGroupHTML::CForm::CEntry entry;
					entry.Name = name;
					entry.Value = decodeHTMLEntities(ucValue);
					entry.Checkbox = checkbox;
					_Forms.back().Entries.push_back (entry);
				}
				renderPseudoElement(":after", elm);
			}
			else if (type == "hidden")
			{
				if (elm.hasNonEmptyAttribute("name"))
				{
					// Get the name
					string name = elm.getAttribute("name");

					// Get the value
					ucstring ucValue;
					ucValue.fromUtf8(elm.getAttribute("value"));

					// Add an entry
					CGroupHTML::CForm::CEntry entry;
					entry.Name = name;
					entry.Value = decodeHTMLEntities(ucValue);
					_Forms.back().Entries.push_back (entry);
				}
			}
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlLI(const CHtmlElement &elm)
	{
		if (_UL.empty())
			return;

		// UL, OL top margin if this is the first LI
		if (!_LI)
		{
			_LI = true;
			newParagraph(ULBeginSpace);
		}
		else
		{
			newParagraph(LIBeginSpace);
		}

		// OL list index can be overridden by <li value="1"> attribute
		if (elm.hasNonEmptyAttribute("value"))
			fromString(elm.getAttribute("value"), _UL.back().Value);

		ucstring str;
		str.fromUtf8(_UL.back().getListMarkerText());
		addString (str);

		// list-style-type: outside
		if (_CurrentViewLink)
		{
			getParagraph()->setFirstViewIndent(-_CurrentViewLink->getMaxUsedW());
		}

		flushString ();

		// after marker
		renderPseudoElement(":before", elm);

		_UL.back().Value++;
	}

	void CGroupHTML::htmlLIend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);
	}

	// ***************************************************************************
	void CGroupHTML::htmlLUA(const CHtmlElement &elm)
	{
		// we receive an embeded lua script
		_ParsingLua = _TrustedDomain; // Only parse lua if TrustedDomain
		_LuaScript.clear();
	}
	
	void CGroupHTML::htmlLUAend(const CHtmlElement &elm)
	{
		if (_ParsingLua && _TrustedDomain)
		{
			_ParsingLua = false;
			// execute the embeded lua script
			_LuaScript = "\nlocal __CURRENT_WINDOW__=\""+this->_Id+"\" \n"+_LuaScript;
			CLuaManager::getInstance().executeLuaScript(_LuaScript, true);
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlMETA(const CHtmlElement &elm)
	{
		if (!_ReadingHeadTag)
			return;

		std::string httpEquiv = elm.getAttribute("http-equiv");
		std::string httpContent = elm.getAttribute("content");
		if (!httpEquiv.empty() && !httpContent.empty())
		{
			// only first http-equiv="refresh" should be handled
			if (_RefreshUrl.empty() && httpEquiv == "refresh")
			{
				const CWidgetManager::SInterfaceTimes &times = CWidgetManager::getInstance()->getInterfaceTimes();
				double timeSec = times.thisFrameMs / 1000.0f;

				string::size_type pos = httpContent.find_first_of(";");
				if (pos == string::npos)
				{
					fromString(httpContent, _NextRefreshTime);
					_RefreshUrl = _URL;
				}
				else
				{
					fromString(httpContent.substr(0, pos), _NextRefreshTime);

					pos = toLower(httpContent).find("url=");
					if (pos != string::npos)
						_RefreshUrl = getAbsoluteUrl(httpContent.substr(pos + 4));
				}

				_NextRefreshTime += timeSec;
			}
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlOBJECT(const CHtmlElement &elm)
	{
		_ObjectType = elm.getAttribute("type");
		_ObjectData = elm.getAttribute("data");
		_ObjectMD5Sum = elm.getAttribute("id");
		_ObjectAction = elm.getAttribute("standby");
		_Object = true;
	}

	void CGroupHTML::htmlOBJECTend(const CHtmlElement &elm)
	{
		if (!_TrustedDomain)
			return;

		if (_ObjectType=="application/ryzom-data")
		{
			if (!_ObjectData.empty())
			{
				if (addBnpDownload(_ObjectData, _ObjectAction, _ObjectScript, _ObjectMD5Sum))
				{
					CLuaManager::getInstance().executeLuaScript("\nlocal __ALLREADYDL__=true\n"+_ObjectScript, true);
				}
				_ObjectScript.clear();
			}
		}
		_Object = false;
	}

	// ***************************************************************************
	void CGroupHTML::htmlOL(const CHtmlElement &elm)
	{
		sint32 start = 1;
		std::string type("1");

		if (elm.hasNonEmptyAttribute("start"))
			fromString(elm.getAttribute("start"), start);
		if (elm.hasNonEmptyAttribute("type"))
			type = elm.getAttribute("type");

		_UL.push_back(HTMLOListElement(start, type));
		// if LI is already present
		_LI = _UL.size() > 1 || _DL.size() > 1;
		_Indent.push_back(getIndent() + ULIndent);
		endParagraph();

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlOLend(const CHtmlElement &elm)
	{
		htmlULend(elm);
	}

	// ***************************************************************************
	void CGroupHTML::htmlOPTION(const CHtmlElement &elm)
	{
		_SelectOption = true;
		_SelectOptionStr.clear();

		// Got one form ?
		if (_Forms.empty() || _Forms.back().Entries.empty())
			return;

		_Forms.back().Entries.back().SelectValues.push_back(elm.getAttribute("value"));

		if (elm.hasAttribute("selected"))
			_Forms.back().Entries.back().InitialSelection = (sint)_Forms.back().Entries.back().SelectValues.size() - 1;

		if (elm.hasAttribute("disabled"))
			_Forms.back().Entries.back().sbOptionDisabled = (sint)_Forms.back().Entries.back().SelectValues.size() - 1;
	}

	void CGroupHTML::htmlOPTIONend(const CHtmlElement &elm)
	{
		if (_Forms.empty() || _Forms.back().Entries.empty())
			return;

		// insert the parsed text into the select control
		CDBGroupComboBox *cb = _Forms.back().Entries.back().ComboBox;
		if (cb)
		{
			uint lineIndex = cb->getNumTexts();
			cb->addText(_SelectOptionStr);
			if (_Forms.back().Entries.back().sbOptionDisabled == lineIndex)
			{
				cb->setGrayed(lineIndex, true);
			}
		}
		else
		{
			CGroupMenu *sb = _Forms.back().Entries.back().SelectBox;
			if (sb)
			{
				uint lineIndex = sb->getNumLine();
				sb->addLine(_SelectOptionStr, "", "");

				if (_Forms.back().Entries.back().sbOptionDisabled == lineIndex)
				{
					sb->setGrayedLine(lineIndex, true);
				}
				else
				{
					// create option line checkbox, CGroupMenu is taking ownership of the checbox
					CInterfaceGroup *ig = CWidgetManager::getInstance()->getParser()->createGroupInstance("menu_checkbox", "", NULL, 0);
					if (ig)
					{
						CCtrlButton *cb = dynamic_cast<CCtrlButton *>(ig->getCtrl("b"));
						if (cb)
						{
							if (_Forms.back().Entries.back().sbMultiple)
							{
								cb->setType(CCtrlButton::ToggleButton);
								cb->setTexture(DefaultCheckBoxBitmapNormal);
								cb->setTexturePushed(DefaultCheckBoxBitmapPushed);
								cb->setTextureOver(DefaultCheckBoxBitmapOver);
							}
							else
							{
								cb->setType(CCtrlButton::RadioButton);
								cb->setTexture(DefaultRadioButtonBitmapNormal);
								cb->setTexturePushed(DefaultRadioButtonBitmapPushed);
								cb->setTextureOver(DefaultRadioButtonBitmapOver);

								if (_Forms.back().Entries.back().sbRBRef == NULL)
									_Forms.back().Entries.back().sbRBRef = cb;

								cb->initRBRefFromRadioButton(_Forms.back().Entries.back().sbRBRef);
							}

							cb->setPushed(_Forms.back().Entries.back().InitialSelection == lineIndex);
							sb->setUserGroupLeft(lineIndex, ig);
						}
						else
						{
							nlwarning("Failed to get 'b' element from 'menu_checkbox' template");
							delete ig;
						}
					}
				}
			}
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlP(const CHtmlElement &elm)
	{
		newParagraph(PBeginSpace);
		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlPend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);
		endParagraph();
	}

	// ***************************************************************************
	void CGroupHTML::htmlPRE(const CHtmlElement &elm)
	{
		_PRE.push_back(true);
		newParagraph(0);

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlPREend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);

		endParagraph();
		popIfNotEmpty(_PRE);
	}

	// ***************************************************************************
	void CGroupHTML::htmlSCRIPT(const CHtmlElement &elm)
	{
		_IgnoreText = true;
	}

	void CGroupHTML::htmlSCRIPTend(const CHtmlElement &elm)
	{
		_IgnoreText = false;
	}

	// ***************************************************************************
	void CGroupHTML::htmlSELECT(const CHtmlElement &elm)
	{
		if (_Forms.empty())
			return;

		// A select box
		string name = elm.getAttribute("name");
		bool multiple = elm.hasAttribute("multiple");
		sint32 size = 0;

		if (elm.hasNonEmptyAttribute("size"))
			fromString(elm.getAttribute("size"), size);

		CGroupHTML::CForm::CEntry entry;
		entry.Name = name;
		entry.sbMultiple = multiple;
		if (size > 1 || multiple)
		{
			entry.InitialSelection = -1;
			CGroupMenu *sb = addSelectBox(DefaultFormSelectBoxMenuGroup, name.c_str());
			if (sb)
			{
				if (size < 1)
					size = 4;

				if (_Style.Current.Width > -1)
					sb->setMinW(_Style.Current.Width);

				if (_Style.Current.Height > -1)
					sb->setMinH(_Style.Current.Height);

				sb->setMaxVisibleLine(size);
				sb->setFontSize(_Style.Current.FontSize);
			}

			entry.SelectBox = sb;
		}
		else
		{
			CDBGroupComboBox *cb = addComboBox(DefaultFormSelectGroup, name.c_str());
			entry.ComboBox = cb;

			if (cb)
			{
				// create view text
				cb->updateCoords();
				setTextStyle(cb->getViewText(), _Style.Current);
			}
		}
		_Forms.back().Entries.push_back (entry);
	}

	void CGroupHTML::htmlSELECTend(const CHtmlElement &elm)
	{
		_SelectOption = false;
		if (_Forms.empty() || _Forms.back().Entries.empty())
			return;

		CDBGroupComboBox *cb = _Forms.back().Entries.back().ComboBox;
		if (cb)
		{
			cb->setSelectionNoTrigger(_Forms.back().Entries.back().InitialSelection);
			// TODO: magic padding
			cb->setW(cb->evalContentWidth() + 16);
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlSTYLE(const CHtmlElement &elm)
	{
		_IgnoreText = true;
	}

	void CGroupHTML::htmlSTYLEend(const CHtmlElement &elm)
	{
		_IgnoreText = false;
	}

	// ***************************************************************************
	void CGroupHTML::htmlTABLE(const CHtmlElement &elm)
	{
		// Get cells parameters
		getCellsParameters(elm, false);

		CGroupTable *table = new CGroupTable(TCtorParam());
		table->BgColor = _CellParams.back().BgColor;

		// TODO: border-spacing: 2em;
		{
			if (elm.hasNonEmptyAttribute("cellspacing"))
				fromString(elm.getAttribute("cellspacing"), table->CellSpacing);
			
			// TODO: cssLength, horiz/vert values
			if (_Style.hasStyle("border-spacing"))
				fromString(_Style.getStyle("border-spacing"), table->CellSpacing);

			// overrides border-spacing if set to 'collapse'
			if (_Style.checkStyle("border-collapse", "collapse"))
				table->CellSpacing = 0;
		}

		if (elm.hasNonEmptyAttribute("cellpadding"))
			fromString(elm.getAttribute("cellpadding"), table->CellPadding);

		if (_Style.hasStyle("width"))
			getPercentage(table->ForceWidthMin, table->TableRatio, _Style.getStyle("width").c_str());
		else if (elm.hasNonEmptyAttribute("width"))
			getPercentage (table->ForceWidthMin, table->TableRatio, elm.getAttribute("width").c_str());

		if (_Style.hasStyle("border") || _Style.hasStyle("border-width"))
		{
			table->Border = _Style.Current.BorderWidth;
		}
		else if (elm.hasAttribute("border"))
		{
			std::string s = elm.getAttribute("border");
			if (s.empty())
				table->Border = 1;
			else
				fromString(elm.getAttribute("border"), table->Border);
		}

		if (_Style.hasStyle("border-color"))
		{
			std::string s = toLower(_Style.getStyle("border-color"));
			if (s == "currentcolor")
				table->BorderColor = _Style.Current.TextColor;
			else
				scanHTMLColor(s.c_str(), table->BorderColor);
		}
		else if (elm.hasNonEmptyAttribute("bordercolor"))
		{
			scanHTMLColor(elm.getAttribute("bordercolor").c_str(), table->BorderColor);
		}

		table->setMarginLeft(getIndent());
		addHtmlGroup (table, 0);

		renderPseudoElement(":before", elm);

		_Tables.push_back(table);

		// Add a cell pointer
		_Cells.push_back(NULL);
		_TR.push_back(false);
		_Indent.push_back(0);
	}

	void CGroupHTML::htmlTABLEend(const CHtmlElement &elm)
	{
		popIfNotEmpty(_CellParams);
		popIfNotEmpty(_TR);
		popIfNotEmpty(_Cells);
		popIfNotEmpty(_Tables);
		popIfNotEmpty(_Indent);

		renderPseudoElement(":after", elm);
		endParagraph();
	}

	// ***************************************************************************
	void CGroupHTML::htmlTD(const CHtmlElement &elm)
	{
		// Get cells parameters
		getCellsParameters(elm, true);

		if (elm.ID == HTML_TH)
		{
			if (!_Style.hasStyle("font-weight"))
				_Style.Current.FontWeight = FONT_WEIGHT_BOLD;
			// center if not specified otherwise.
			if (!elm.hasNonEmptyAttribute("align") && !_Style.hasStyle("text-align"))
				_CellParams.back().Align = CGroupCell::Center;
		}

		CGroupTable *table = getTable();
		if (table)
		{
			if (_Style.hasStyle("padding"))
			{
				uint32 a;
				// TODO: cssLength
				if (fromString(_Style.getStyle("padding"), a))
					table->CellPadding = a;
			}

			if (!_Cells.empty())
			{
				_Cells.back() = new CGroupCell(CViewBase::TCtorParam());

				if (_Style.checkStyle("background-repeat", "1") || _Style.checkStyle("background-repeat", "repeat"))
					_Cells.back()->setTextureTile(true);

				if (_Style.checkStyle("background-scale", "1") || _Style.checkStyle("background-size", "cover"))
					_Cells.back()->setTextureScale(true);

				if (_Style.hasStyle("background-image"))
				{
					string image = _Style.getStyle("background-image");

					string::size_type texExt = toLower(image).find("url(");
					// Url image
					if (texExt != string::npos)
					{
						// Remove url()
						image = image.substr(4, image.size()-5);
						addImageDownload(image, _Cells.back());
					// Image in BNP
					}
					else
					{
						_Cells.back()->setTexture(image);
					}
				}

				if (elm.hasNonEmptyAttribute("colspan"))
					fromString(elm.getAttribute("colspan"), _Cells.back()->ColSpan);
				if (elm.hasNonEmptyAttribute("rowspan"))
					fromString(elm.getAttribute("rowspan"), _Cells.back()->RowSpan);

				_Cells.back()->BgColor = _CellParams.back().BgColor;
				_Cells.back()->Align = _CellParams.back().Align;
				_Cells.back()->VAlign = _CellParams.back().VAlign;
				_Cells.back()->LeftMargin = _CellParams.back().LeftMargin;
				_Cells.back()->NoWrap = _CellParams.back().NoWrap;
				_Cells.back()->ColSpan = std::max(1, _Cells.back()->ColSpan);
				_Cells.back()->RowSpan = std::max(1, _Cells.back()->RowSpan);

				float temp;
				if (_Style.hasStyle("width"))
					getPercentage (_Cells.back()->WidthWanted, _Cells.back()->TableRatio, _Style.getStyle("width").c_str());
				else if (elm.hasNonEmptyAttribute("width"))
					getPercentage (_Cells.back()->WidthWanted, _Cells.back()->TableRatio, elm.getAttribute("width").c_str());
				
				if (_Style.hasStyle("height"))
					getPercentage (_Cells.back()->Height, temp, _Style.getStyle("height").c_str());
				else if (elm.hasNonEmptyAttribute("height"))
					getPercentage (_Cells.back()->Height, temp, elm.getAttribute("height").c_str());

				_Cells.back()->NewLine = getTR();
				table->addChild (_Cells.back());

				// reusing indent pushed by table
				_Indent.back() = 0;

				newParagraph(TDBeginSpace);
				// indent is already 0, getParagraph()->setMarginLeft(0); // maybe setIndent(0) if LI is using one

				// Reset TR flag
				if (!_TR.empty())
					_TR.back() = false;

				renderPseudoElement(":before", elm);
			}
		}
	}

	void CGroupHTML::htmlTDend(const CHtmlElement &elm)
	{
		renderPseudoElement(":after", elm);

		popIfNotEmpty(_CellParams);
		if (!_Cells.empty())
			_Cells.back() = NULL;
	}

	// ***************************************************************************
	void CGroupHTML::htmlTEXTAREA(const CHtmlElement &elm)
	{
		_PRE.push_back(true);

		// Got one form ?
		if (!(_Forms.empty()))
		{
			// read general property
			string templateName;

			// Widget template name
			if (elm.hasNonEmptyAttribute("z_input_tmpl"))
				templateName = elm.getAttribute("z_input_tmpl");

			// Get the string name
			_TextAreaName.clear();
			_TextAreaRow = 1;
			_TextAreaCols = 10;
			_TextAreaContent.clear();
			_TextAreaMaxLength = 1024;
			if (elm.hasNonEmptyAttribute("name"))
				_TextAreaName = elm.getAttribute("name");
			if (elm.hasNonEmptyAttribute("rows"))
				fromString(elm.getAttribute("rows"), _TextAreaRow);
			if (elm.hasNonEmptyAttribute("cols"))
				fromString(elm.getAttribute("cols"), _TextAreaCols);
			if (elm.hasNonEmptyAttribute("maxlength"))
				fromString(elm.getAttribute("maxlength"), _TextAreaMaxLength);

			_TextAreaTemplate = !templateName.empty() ? templateName : DefaultFormTextAreaGroup;
			_TextArea = true;
		}
	}

	void CGroupHTML::htmlTEXTAREAend(const CHtmlElement &elm)
	{
		_TextArea = false;
		popIfNotEmpty (_PRE);

		if (_Forms.empty())
			return;

		CInterfaceGroup *textArea = addTextArea (_TextAreaTemplate, _TextAreaName.c_str (), _TextAreaRow, _TextAreaCols, true, _TextAreaContent, _TextAreaMaxLength);
		if (textArea)
		{
			// Add the text area to the form
			CGroupHTML::CForm::CEntry entry;
			entry.Name = _TextAreaName;
			entry.TextArea = textArea;
			_Forms.back().Entries.push_back (entry);
		}
	}

	// ***************************************************************************
	void CGroupHTML::htmlTH(const CHtmlElement &elm)
	{
		htmlTD(elm);
	}

	void CGroupHTML::htmlTHend(const CHtmlElement &elm)
	{
		htmlTDend(elm);
	}

	// ***************************************************************************
	void CGroupHTML::htmlTITLE(const CHtmlElement &elm)
	{
		// TODO: only from <head>
		// if (!_ReadingHeadTag) return;
		if(!_TitlePrefix.empty())
			_TitleString = _TitlePrefix + " - ";
		else
			_TitleString.clear();
		_Title = true;
	}

	void CGroupHTML::htmlTITLEend(const CHtmlElement &elm)
	{
		_Title = false;
		setTitle(_TitleString);
	}

	// ***************************************************************************
	void CGroupHTML::htmlTR(const CHtmlElement &elm)
	{
		// Get cells parameters
		getCellsParameters(elm, true);

		// TODO: this probably ends up in first cell
		renderPseudoElement(":before", elm);

		// Set TR flag
		if (!_TR.empty())
			_TR.back() = true;
	}

	void CGroupHTML::htmlTRend(const CHtmlElement &elm)
	{
		// TODO: this probably ends up in last cell
		renderPseudoElement(":after", elm);

		popIfNotEmpty(_CellParams);
	}

	// ***************************************************************************
	void CGroupHTML::htmlUL(const CHtmlElement &elm)
	{
		if (_UL.empty())
			_UL.push_back(HTMLOListElement(1, "disc"));
		else if (_UL.size() == 1)
			_UL.push_back(HTMLOListElement(1, "circle"));
		else
			_UL.push_back(HTMLOListElement(1, "square"));

		// if LI is already present
		_LI = _UL.size() > 1 || _DL.size() > 1;
		_Indent.push_back(getIndent() + ULIndent);
		endParagraph();

		renderPseudoElement(":before", elm);
	}

	void CGroupHTML::htmlULend(const CHtmlElement &elm)
	{
		if (_UL.empty())
			return;

		renderPseudoElement(":after", elm);

		endParagraph();
		popIfNotEmpty(_UL);
		popIfNotEmpty(_Indent);
	}

}

