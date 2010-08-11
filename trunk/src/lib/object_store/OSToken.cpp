/* $Id$ */

/*
 * Copyright (c) 2010 SURFnet bv
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*****************************************************************************
 OSToken.cpp

 The token class; a token is stored in a directory containing several files.
 Each object is stored in a separate file and a token object is present that
 has the token specific attributes
 *****************************************************************************/

#include "config.h"
#include "log.h"
#include "OSAttributes.h"
#include "OSAttribute.h"
#include "ObjectFile.h"
#include "Directory.h"
#include "UUID.h"
#include "IPCSignal.h"
#include "cryptoki.h"
#include "OSToken.h"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <list>

// Constructor
OSToken::OSToken(const std::string tokenPath)
{
	tokenDir = new Directory(tokenPath);
	tokenObject = new ObjectFile(tokenPath + "/tokenObject");
	sync = IPCSignal::create(tokenPath);
	tokenMutex = MutexFactory::i()->getMutex();
	this->tokenPath = tokenPath;
	valid = (sync != NULL) && (tokenMutex != NULL) && tokenDir->isValid() && tokenObject->isValid();

	index(true);
}

// Create a new token
/*static*/ OSToken* OSToken::createToken(const std::string basePath, const std::string tokenDir, const ByteString& label, const ByteString& serial)
{
	Directory baseDir(basePath);

	if (!baseDir.isValid())
	{
		return NULL;
	}

	// Create the token directory
	if (!baseDir.mkdir(tokenDir))
	{
		return NULL;
	}

	// Create the token object
	ObjectFile tokenObject(basePath + "/" + tokenDir + "/tokenObject", true);

	if (!tokenObject.isValid())
	{
		baseDir.remove(tokenDir);

		return NULL;
	}

	// Set the initial attributes
	CK_ULONG flags = 
		CKF_RNG |
		CKF_LOGIN_REQUIRED | // FIXME: check
		CKF_RESTORE_KEY_NOT_NEEDED |
		CKF_TOKEN_INITIALIZED |
		CKF_SO_PIN_LOCKED |
		CKF_SO_PIN_TO_BE_CHANGED;

	OSAttribute tokenLabel(label);
	OSAttribute tokenSerial(serial);
	OSAttribute tokenFlags(flags);

	if (!tokenObject.setAttribute(CKA_OS_TOKENLABEL, tokenLabel) ||
	    !tokenObject.setAttribute(CKA_OS_TOKENSERIAL, tokenSerial) ||
	    !tokenObject.setAttribute(CKA_OS_TOKENFLAGS, tokenFlags))
	{
		baseDir.remove(tokenDir + "/tokenObject");
		baseDir.remove(tokenDir);

		return NULL;
	}

	return new OSToken(basePath + "/" + tokenDir);
}

// Destructor
OSToken::~OSToken()
{
	// Clean up
	std::set<ObjectFile*> cleanUp = allObjects;
	allObjects.clear();

	for (std::set<ObjectFile*>::iterator i = cleanUp.begin(); i != cleanUp.end(); i++)
	{
		delete *i;
	}

	delete tokenDir;
	if (sync != NULL) delete sync;
	if (tokenMutex != NULL) delete tokenMutex;
	delete tokenObject;
}

// Set the SO PIN
bool OSToken::setSOPIN(const ByteString& soPINBlob)
{
	OSAttribute soPIN(soPINBlob);

	tokenObject->setAttribute(CKA_OS_SOPIN, soPIN);
}

// Get the SO PIN
bool OSToken::getSOPIN(ByteString& soPINBlob)
{
	if (!tokenObject->isValid())
	{
		return false;
	}

	OSAttribute* soPIN = tokenObject->getAttribute(CKA_OS_SOPIN);

	if (soPIN != NULL)
	{
		soPINBlob = soPIN->getByteStringValue();

		return true;
	}
	else
	{
		return false;
	}
}

// Set the user PIN
bool OSToken::setUserPIN(ByteString userPINBlob)
{
	OSAttribute userPIN(userPINBlob);

	return tokenObject->setAttribute(CKA_OS_USERPIN, userPIN);
}

// Get the user PIN
bool OSToken::getUserPIN(ByteString& userPINBlob)
{
	if (!tokenObject->isValid())
	{
		return false;
	}

	OSAttribute* userPIN = tokenObject->getAttribute(CKA_OS_USERPIN);

	if (userPIN != NULL)
	{
		userPINBlob = userPIN->getByteStringValue();

		return true;
	}
	else
	{
		return false;
	}
}

// Get the token flags
bool OSToken::getTokenFlags(CK_ULONG& flags)
{
	if (!tokenObject->isValid())
	{
		return false;
	}

	OSAttribute* tokenFlags = tokenObject->getAttribute(CKA_OS_TOKENFLAGS);

	if (tokenFlags != NULL)
	{
		flags = tokenFlags->getUnsignedLongValue();

		// Check if the user PIN is initialised
		if (tokenObject->attributeExists(CKA_OS_USERPIN))
		{
			flags |= CKF_USER_PIN_INITIALIZED;
		}

		return true;
	}
	else
	{
		return false;
	}
}

// Set the token flags
bool OSToken::setTokenFlags(const CK_ULONG flags)
{
	OSAttribute tokenFlags(flags);

	return tokenObject->setAttribute(CKA_OS_TOKENFLAGS, tokenFlags);
}

// Retrieve objects
std::set<ObjectFile*> OSToken::getObjects()
{
	index();

	// Make sure that no other thread is in the process of changing
	// the object list when we return it
	MutexLocker lock(tokenMutex);

	return objects;
}

// Checks if the token is consistent
bool OSToken::isValid()
{
	return valid;
}

// Index the token
bool OSToken::index(bool isFirstTime /* = false */)
{
	// Check if re-indexing is required
	if (!isFirstTime && (!valid || !sync->wasTriggered()))
	{
		return true;
	}

	// Check the integrity
	if (!tokenDir->refresh() || !tokenObject->isValid())
	{
		valid = false;

		return false;
	}

	// Retrieve the directory listing
	std::vector<std::string> tokenFiles = tokenDir->getFiles();

	// Filter out the objects
	std::set<std::string> newSet;

	for (std::vector<std::string>::iterator i = tokenFiles.begin(); i != tokenFiles.end(); i++)
	{
		if ((i->size() > 7) && (!(i->substr(i->size() - 7).compare(".object"))))
		{
			newSet.insert(*i);
		}
	}

	// Compute the changes compared to the last list of files
	std::set<std::string> addedFiles;
	std::set<std::string> removedFiles;

	if (!isFirstTime)
	{
		// First compute which files were added
		for (std::set<std::string>::iterator i = newSet.begin(); i != newSet.end(); i++)
		{
			if (currentFiles.find(*i) == currentFiles.end())
			{
				addedFiles.insert(*i);
			}
		}

		// Now compute which files were removed
		for (std::set<std::string>::iterator i = currentFiles.begin(); i != currentFiles.end(); i++)
		{
			if (newSet.find(*i) == newSet.end())
			{
				removedFiles.insert(*i);
			}
		}
	}
	else
	{
		addedFiles = newSet;
	}

	// Now update the set of objects
	MutexLocker lock(tokenMutex);

	// Add new objects
	for (std::set<std::string>::iterator i = addedFiles.begin(); i != addedFiles.end(); i++)
	{
		// Create a new token object for the added file
		ObjectFile* newObject = new ObjectFile(tokenPath + "/" + *i);
		newObject->linkToken(this);

		objects.insert(newObject);
		allObjects.insert(newObject);
	}

	// Remove deleted objects
	std::set<ObjectFile*> newObjects;

	for (std::set<ObjectFile*>::iterator i = objects.begin(); i != objects.end(); i++)
	{
		if (removedFiles.find((*i)->getFilename()) == removedFiles.end())
		{
			// This object gets to stay in the set
			newObjects.insert(*i);
		}
	}

	// Set the new objects
	objects = newObjects;

	return true;
}

