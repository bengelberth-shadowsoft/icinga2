/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "icinga/service.h"
#include "base/dynamictype.h"
#include "base/objectlock.h"
#include "base/logger_fwd.h"
#include "base/timer.h"
#include <boost/tuple/tuple.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/foreach.hpp>

using namespace icinga;

static int l_NextDowntimeID = 1;
static boost::mutex l_DowntimeMutex;
static std::map<int, String> l_LegacyDowntimesCache;
static std::map<String, Service::WeakPtr> l_DowntimesCache;
static bool l_DowntimesCacheNeedsUpdate = false;
static Timer::Ptr l_DowntimesCacheTimer;
static Timer::Ptr l_DowntimesExpireTimer;

/**
 * @threadsafety Always.
 */
int Service::GetNextDowntimeID(void)
{
	boost::mutex::scoped_lock lock(l_DowntimeMutex);

	return l_NextDowntimeID;
}

/**
 * @threadsafety Always.
 */
Dictionary::Ptr Service::GetDowntimes(void) const
{
	return m_Downtimes;
}

/**
 * @threadsafety Always.
 */
String Service::AddDowntime(const String& author, const String& comment,
    double startTime, double endTime, bool fixed,
    const String& triggeredBy, double duration)
{
	Dictionary::Ptr downtime = boost::make_shared<Dictionary>();
	downtime->Set("entry_time", Utility::GetTime());
	downtime->Set("author", author);
	downtime->Set("comment", comment);
	downtime->Set("start_time", startTime);
	downtime->Set("end_time", endTime);
	downtime->Set("fixed", fixed);
	downtime->Set("duration", duration);
	downtime->Set("triggered_by", triggeredBy);
	downtime->Set("triggers", boost::make_shared<Dictionary>());
	downtime->Set("trigger_time", 0);

	int legacy_id;

	{
		boost::mutex::scoped_lock lock(l_DowntimeMutex);
		legacy_id = l_NextDowntimeID++;
	}

	downtime->Set("legacy_id", legacy_id);

	if (!triggeredBy.IsEmpty()) {
		Service::Ptr otherOwner = GetOwnerByDowntimeID(triggeredBy);
		Dictionary::Ptr otherDowntimes = otherOwner->Get("downtimes");
		Dictionary::Ptr otherDowntime = otherDowntimes->Get(triggeredBy);
		Dictionary::Ptr triggers = otherDowntime->Get("triggers");
		triggers->Set(triggeredBy, triggeredBy);
		otherOwner->Touch("downtimes");
	}

	Dictionary::Ptr downtimes;

	{
		ObjectLock olock(this);

		downtimes = m_Downtimes;

		if (!downtimes)
			downtimes = boost::make_shared<Dictionary>();

		m_Downtimes = downtimes;
	}

	String id = Utility::NewUUID();
	downtimes->Set(id, downtime);

	Touch("downtimes");

	return id;
}

/**
 * @threadsafety Always.
 */
void Service::RemoveDowntime(const String& id)
{
	Service::Ptr owner = GetOwnerByDowntimeID(id);

	if (!owner)
		return;

	Dictionary::Ptr downtimes = owner->GetDowntimes();

	if (!downtimes)
		return;

	downtimes->Remove(id);
	owner->Touch("downtimes");
}

/**
 * @threadsafety Always.
 */
void Service::TriggerDowntimes(void)
{
	Dictionary::Ptr downtimes = GetDowntimes();

	if (!downtimes)
		return;

	ObjectLock olock(downtimes);

	String id;
	BOOST_FOREACH(boost::tie(id, boost::tuples::ignore), downtimes) {
		TriggerDowntime(id);
	}
}

/**
 * @threadsafety Always.
 */
void Service::TriggerDowntime(const String& id)
{
	Service::Ptr owner = GetOwnerByDowntimeID(id);
	Dictionary::Ptr downtime = GetDowntimeByID(id);

	if (!downtime)
		return;

	double now = Utility::GetTime();

	if (now < downtime->Get("start_time") ||
	    now > downtime->Get("end_time"))
		return;

	if (downtime->Get("trigger_time") == 0)
		downtime->Set("trigger_time", now);

	Dictionary::Ptr triggers = downtime->Get("triggers");
	ObjectLock olock(triggers);
	String tid;
	BOOST_FOREACH(boost::tie(tid, boost::tuples::ignore), triggers) {
		TriggerDowntime(tid);
	}

	owner->Touch("downtimes");
}

/**
 * @threadsafety Always.
 */
String Service::GetDowntimeIDFromLegacyID(int id)
{
	boost::mutex::scoped_lock lock(l_DowntimeMutex);

	std::map<int, String>::iterator it = l_LegacyDowntimesCache.find(id);

	if (it == l_LegacyDowntimesCache.end())
		return Empty;

	return it->second;
}

/**
 * @threadsafety Always.
 */
Service::Ptr Service::GetOwnerByDowntimeID(const String& id)
{
	boost::mutex::scoped_lock lock(l_DowntimeMutex);
	return l_DowntimesCache[id].lock();
}

/**
 * @threadsafety Always.
 */
Dictionary::Ptr Service::GetDowntimeByID(const String& id)
{
	Service::Ptr owner = GetOwnerByDowntimeID(id);

	if (!owner)
		return Dictionary::Ptr();

	Dictionary::Ptr downtimes = owner->GetDowntimes();

	if (downtimes)
		return downtimes->Get(id);

	return Dictionary::Ptr();
}

/**
 * @threadsafety Always.
 */
bool Service::IsDowntimeActive(const Dictionary::Ptr& downtime)
{
	double now = Utility::GetTime();

	if (now < downtime->Get("start_time") ||
	    now > downtime->Get("end_time"))
		return false;

	if (static_cast<bool>(downtime->Get("fixed")))
		return true;

	double triggerTime = downtime->Get("trigger_time");

	if (triggerTime == 0)
		return false;

	return (triggerTime + downtime->Get("duration") < now);
}

/**
 * @threadsafety Always.
 */
bool Service::IsDowntimeExpired(const Dictionary::Ptr& downtime)
{
	return (downtime->Get("end_time") < Utility::GetTime());
}

/**
 * @threadsafety Always.
 */
void Service::InvalidateDowntimesCache(void)
{
	boost::mutex::scoped_lock lock(l_DowntimeMutex);

	if (l_DowntimesCacheNeedsUpdate)
		return; /* Someone else has already requested a refresh. */

	if (!l_DowntimesCacheTimer) {
		l_DowntimesCacheTimer = boost::make_shared<Timer>();
		l_DowntimesCacheTimer->SetInterval(0.5);
		l_DowntimesCacheTimer->OnTimerExpired.connect(boost::bind(&Service::RefreshDowntimesCache));
		l_DowntimesCacheTimer->Start();
	}

	l_DowntimesCacheNeedsUpdate = true;
}

/**
 * @threadsafety Always.
 */
void Service::RefreshDowntimesCache(void)
{
	{
		boost::mutex::scoped_lock lock(l_DowntimeMutex);

		if (!l_DowntimesCacheNeedsUpdate)
			return;

		l_DowntimesCacheNeedsUpdate = false;
	}

	Log(LogDebug, "icinga", "Updating Service downtimes cache.");

	std::map<int, String> newLegacyDowntimesCache;
	std::map<String, Service::WeakPtr> newDowntimesCache;

	BOOST_FOREACH(const DynamicObject::Ptr& object, DynamicType::GetObjects("Service")) {
		Service::Ptr service = dynamic_pointer_cast<Service>(object);

		Dictionary::Ptr downtimes = service->GetDowntimes();

		if (!downtimes)
			continue;

		ObjectLock olock(downtimes);

		String id;
		Dictionary::Ptr downtime;
		BOOST_FOREACH(boost::tie(id, downtime), downtimes) {
			int legacy_id = downtime->Get("legacy_id");

			if (legacy_id >= l_NextDowntimeID)
				l_NextDowntimeID = legacy_id + 1;

			if (newLegacyDowntimesCache.find(legacy_id) != newLegacyDowntimesCache.end()) {
				/* The legacy_id is already in use by another downtime;
				 * this shouldn't usually happen - assign it a new ID. */
				legacy_id = l_NextDowntimeID++;
				downtime->Set("legacy_id", legacy_id);
				service->Touch("downtimes");
			}

			newLegacyDowntimesCache[legacy_id] = id;
			newDowntimesCache[id] = service;
		}
	}

	boost::mutex::scoped_lock lock(l_DowntimeMutex);

	l_DowntimesCache.swap(newDowntimesCache);
	l_LegacyDowntimesCache.swap(newLegacyDowntimesCache);

	if (!l_DowntimesExpireTimer) {
		l_DowntimesExpireTimer = boost::make_shared<Timer>();
		l_DowntimesExpireTimer->SetInterval(300);
		l_DowntimesExpireTimer->OnTimerExpired.connect(boost::bind(&Service::DowntimesExpireTimerHandler));
		l_DowntimesExpireTimer->Start();
	}
}

/**
 * @threadsafety Always.
 */
void Service::RemoveExpiredDowntimes(void)
{
	Dictionary::Ptr downtimes = GetDowntimes();

	if (!downtimes)
		return;

	std::vector<String> expiredDowntimes;

	{
		ObjectLock olock(downtimes);

		String id;
		Dictionary::Ptr downtime;
		BOOST_FOREACH(boost::tie(id, downtime), downtimes) {
			if (IsDowntimeExpired(downtime))
				expiredDowntimes.push_back(id);
		}
	}

	if (!expiredDowntimes.empty()) {
		BOOST_FOREACH(const String& id, expiredDowntimes) {
			downtimes->Remove(id);
		}

		Touch("downtimes");
	}
}

/**
 * @threadsafety Always.
 */
void Service::DowntimesExpireTimerHandler(void)
{
	BOOST_FOREACH(const DynamicObject::Ptr& object, DynamicType::GetObjects("Service")) {
		Service::Ptr service = dynamic_pointer_cast<Service>(object);
		service->RemoveExpiredDowntimes();
	}
}

/**
 * @threadsafety Always.
 */
bool Service::IsInDowntime(void) const
{
	Dictionary::Ptr downtimes = GetDowntimes();

	if (!downtimes)
		return false;

	ObjectLock olock(downtimes);

	Dictionary::Ptr downtime;
	BOOST_FOREACH(boost::tie(boost::tuples::ignore, downtime), downtimes) {
		if (Service::IsDowntimeActive(downtime))
			return true;
	}

	return false;
}
