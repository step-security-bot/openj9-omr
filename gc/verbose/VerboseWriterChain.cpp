/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "VerboseWriterChain.hpp"

#include "VerboseBuffer.hpp"
#include "VerboseWriter.hpp"

#include "GCExtensionsBase.hpp"

#undef _UTE_MODULE_HEADER_
#undef UT_MODULE_LOADED
#undef UT_MODULE_UNLOADED
#include "ut_j9vgc.h"

MM_VerboseWriterChain::MM_VerboseWriterChain()
	: MM_Base()
	,_buffer(NULL)
	,_writers(NULL)
{}

MM_VerboseWriterChain *
MM_VerboseWriterChain::newInstance(MM_EnvironmentBase *env)
{
	MM_GCExtensionsBase *extensions = env->getExtensions();
	
	MM_VerboseWriterChain *chain = (MM_VerboseWriterChain *)extensions->getForge()->allocate(sizeof(MM_VerboseWriterChain), OMR::GC::AllocationCategory::DIAGNOSTIC, OMR_GET_CALLSITE());
	if(NULL != chain) {
		new(chain) MM_VerboseWriterChain();
		if(!chain->initialize(env)) {
			chain->kill(env);
			chain = NULL;
		}
	}
	return chain;
}

void
MM_VerboseWriterChain::formatAndOutput(MM_EnvironmentBase *env, uintptr_t indent, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	_buffer->formatAndOutputV(env, indent, format, args);
	va_end(args);
}

void
MM_VerboseWriterChain::flush(MM_EnvironmentBase *env)
{
	MM_VerboseWriter* writer = _writers;
	while (NULL != writer) {
		writer->outputString(env, _buffer->contents());
		writer = writer->getNextWriter();
	}
	_buffer->reset();
}

void
MM_VerboseWriterChain::tearDown(MM_EnvironmentBase* env)
{
	if (NULL != _buffer) {
		_buffer->kill(env);
		_buffer = NULL;
	}
	MM_VerboseWriter* writer = _writers;
	while (NULL != writer) {
		MM_VerboseWriter* nextWriter = writer->getNextWriter();
		writer->kill(env);
		writer = nextWriter;
	}
	_writers = NULL;
}

void
MM_VerboseWriterChain::kill(MM_EnvironmentBase* env) {
	tearDown(env);
	env->getExtensions()->getForge()->free(this);
}

bool
MM_VerboseWriterChain::initialize(MM_EnvironmentBase* env)
{
	bool result = true;

	_buffer = MM_VerboseBuffer::newInstance(env, INITIAL_BUFFER_SIZE);
	if(NULL == _buffer) {
		result = false;
	}
	
	return result;
}

void
MM_VerboseWriterChain::addWriter(MM_VerboseWriter* writer)
{
	writer->setNextWriter(_writers);
	_writers = writer;
}

void
MM_VerboseWriterChain::endOfCycle(MM_EnvironmentBase *env)
{
	MM_VerboseWriter* writer = _writers;
	while (NULL != writer) {
		writer->endOfCycle(env);
		writer = writer->getNextWriter();
	}
}


