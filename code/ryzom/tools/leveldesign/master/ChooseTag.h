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

#if !defined(AFX_CHOOSETAG_H__4185965E_4D29_491F_8601_7B29BBF28328__INCLUDED_)
#define AFX_CHOOSETAG_H__4185965E_4D29_491F_8601_7B29BBF28328__INCLUDED_

#include <string>
#include <vector>

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
// ChooseTag.h : header file
//

/////////////////////////////////////////////////////////////////////////////
// CChooseTag dialog

class CChooseTag : public CDialog
{
	std::string _Path;
	std::vector<std::string> _Names;
	int _Sel;
// Construction
public:
	CChooseTag (CWnd* pParent = NULL);   // standard constructor
	void setPath (const std::string &path);
	const char *getSelected ();

// Dialog Data
	//{{AFX_DATA(CChooseTag)
	enum { IDD = IDD_CHOOSETAG };
		// NOTE: the ClassWizard will add data members here
	//}}AFX_DATA

	BOOL OnInitDialog ();

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CChooseTag)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:

	// Generated message map functions
	//{{AFX_MSG(CChooseTag)
	afx_msg void OnSelchangeList();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_CHOOSETAG_H__4185965E_4D29_491F_8601_7B29BBF28328__INCLUDED_)
