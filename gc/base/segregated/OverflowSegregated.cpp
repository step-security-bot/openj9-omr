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
#include "omr.h"

#include "CollectorLanguageInterface.hpp"
#include "EnvironmentBase.hpp"
#include "GCExtensionsBase.hpp"
#include "Heap.hpp"
#include "HeapRegionIterator.hpp"
#include "MarkingScheme.hpp"
#include "MarkMap.hpp"
#include "ObjectHeapIteratorSegregated.hpp"
#include "Packet.hpp"
#include "SegregatedGC.hpp"
#include "SegregatedMarkingScheme.hpp"
#include "Task.hpp"
#include "WorkPackets.hpp"

#include "OverflowSegregated.hpp"

#if defined(OMR_GC_SEGREGATED_HEAP)

/**
 * Create a new MM_OverflowSegregated object
 */
MM_OverflowSegregated *
MM_OverflowSegregated::newInstance(MM_EnvironmentBase *env, MM_WorkPackets *workPackets)
{
	Assert_MM_true(env->getExtensions()->isSegregatedHeap());
	MM_OverflowSegregated *overflow = (MM_OverflowSegregated *)env->getForge()->allocate(sizeof(MM_OverflowSegregated), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if (NULL != overflow) {
		new(overflow) MM_OverflowSegregated(env,workPackets);
		if (!overflow->initialize(env)) {
			overflow->kill(env);
			overflow = NULL;
		}
	}
	return overflow;
}

bool
MM_OverflowSegregated::initialize(MM_EnvironmentBase *env)
{
	return MM_WorkPacketOverflow::initialize(env);
}

void
MM_OverflowSegregated::tearDown(MM_EnvironmentBase *env)
{
	MM_WorkPacketOverflow::tearDown(env);
}

void
MM_OverflowSegregated::emptyToOverflow(MM_EnvironmentBase *env, MM_Packet *packet, MM_OverflowType type)
{
	void *objectPtr;

	_overflow = true;

	_extensions->globalGCStats.workPacketStats.setSTWWorkStackOverflowOccured(true);
	_extensions->globalGCStats.workPacketStats.incrementSTWWorkStackOverflowCount();
	_extensions->globalGCStats.workPacketStats.setSTWWorkpacketCountAtOverflow(_workPackets->getActivePacketCount());

	/* Empty the current packet by dirtying its cards now */
	while(NULL != (objectPtr = packet->pop(env))) {
		overflowItemInternal(env, objectPtr);
	}

	Assert_MM_true(packet->isEmpty());
}

void
MM_OverflowSegregated::overflowItem(MM_EnvironmentBase *env, void *item, MM_OverflowType type)
{
	_overflow = true;

	_extensions->globalGCStats.workPacketStats.setSTWWorkStackOverflowOccured(true);
	_extensions->globalGCStats.workPacketStats.incrementSTWWorkStackOverflowCount();
	_extensions->globalGCStats.workPacketStats.setSTWWorkpacketCountAtOverflow(_workPackets->getActivePacketCount());

	overflowItemInternal(env, item);
}

void
MM_OverflowSegregated::overflowItemInternal(MM_EnvironmentBase *env, void *item)
{
	void *heapBase = _extensions->heap->getHeapBase();
	void *heapTop = _extensions->heap->getHeapTop();

	if ((PACKET_ARRAY_SPLIT_TAG != ((uintptr_t)item & PACKET_ARRAY_SPLIT_TAG)) &&  (item >= heapBase) && (item < heapTop)) {
		MM_SegregatedGC *globalCollector = (MM_SegregatedGC *)_extensions->getGlobalCollector();
		MM_SegregatedMarkingScheme *markingScheme = globalCollector->getMarkingScheme();
		MM_MarkMap *markMap = markingScheme->getMarkMap();
		omrobjectptr_t objectPtr = (omrobjectptr_t)item;

		/* object has to be marked already */
		Assert_MM_true(markMap->isBitSet(objectPtr));
		Assert_MM_false(markMap->isBitSet((omrobjectptr_t)((uintptr_t)item + markMap->getObjectGrain())));

		/* set overflow bit (double marking) */
		markMap->atomicSetBit((omrobjectptr_t)((uintptr_t)item + markMap->getObjectGrain()));

		/* Perform language specific actions */
		markingScheme->getMarkingDelegate()->handleWorkPacketOverflowItem(env,objectPtr);
	}
}

void
MM_OverflowSegregated::fillFromOverflow(MM_EnvironmentBase *env, MM_Packet *packet)
{
	Assert_MM_unreachable();
}

void
MM_OverflowSegregated::handleOverflow(MM_EnvironmentBase *env)
{
	if (env->_currentTask->synchronizeGCThreadsAndReleaseMain(env, UNIQUE_ID)) {
		_overflow = false;

		MM_Heap *heap = _extensions->heap;
		MM_HeapRegionManager *regionManager = heap->getHeapRegionManager();
		GC_HeapRegionIterator regionIterator(regionManager);
		MM_HeapRegionDescriptorSegregated *region = NULL;
		MM_SegregatedGC *globalCollector = (MM_SegregatedGC *)_extensions->getGlobalCollector();
		MM_SegregatedMarkingScheme *markingScheme = globalCollector->getMarkingScheme();
		MM_MarkMap *markMap = markingScheme->getMarkMap();

		while((region = (MM_HeapRegionDescriptorSegregated *)regionIterator.nextRegion()) != NULL) {
			GC_ObjectHeapIteratorSegregated objectIterator(_extensions, (omrobjectptr_t)region->getLowAddress(), (omrobjectptr_t)region->getHighAddress(), region->getRegionType(), region->getCellSize(), false, false);
			omrobjectptr_t object;

			while((object = objectIterator.nextObject()) != NULL) {
				/* search for double marked (overflowed) objects */
				if ((markMap->isBitSet(object)) && (markMap->isBitSet((omrobjectptr_t)((uintptr_t)object + markMap->getObjectGrain())))) {
					/* clean overflow mark */
					markMap->clearBit((omrobjectptr_t)((uintptr_t)object + markMap->getObjectGrain()));

					/* scan overflowed object */
					/* TODO Fix this as it is wrong for metronome GC policy */
					/* The subclassing of markingscheme by segregatedmarking scheme needs a lot of work and means metronome is pretty broken */
					markingScheme->scanObject(env, object, SCAN_REASON_OVERFLOWED_OBJECT);
				}
			}
		}
		env->_currentTask->releaseSynchronizedGCThreads(env);
	}
}

void
MM_OverflowSegregated::reset(MM_EnvironmentBase *env)
{

}

bool
MM_OverflowSegregated::isEmpty()
{
	return true;
}

#endif /* OMR_GC_SEGREGATED_HEAP */
