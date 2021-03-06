/****************************************************************************
 *
 *   Copyright (c) 2013-2014 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @file navigator_mission.cpp
 *
 * Helper class to access missions
 *
 * @author Julian Oes <julian@oes.ch>
 */

#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <drivers/drv_hrt.h>

#include <dataman/dataman.h>
#include <mavlink/mavlink_log.h>
#include <systemlib/err.h>
#include <geo/geo.h>

#include <uORB/uORB.h>
#include <uORB/topics/mission.h>
#include <uORB/topics/mission_result.h>

#include "mission.h"
#include "navigator.h"

Mission::Mission(Navigator *navigator, const char *name) :
	MissionBlock(navigator, name),
	_param_onboard_enabled(this, "ONBOARD_EN"),
	_param_takeoff_alt(this, "TAKEOFF_ALT"),
	_onboard_mission({0}),
	_offboard_mission({0}),
	_current_onboard_mission_index(-1),
	_current_offboard_mission_index(-1),
	_need_takeoff(true),
	_takeoff(false),
	_mission_result_pub(-1),
	_mission_result({0}),
	_mission_type(MISSION_TYPE_NONE)
{
	/* load initial params */
	updateParams();

	/* set initial mission items */
	on_inactive();
}

Mission::~Mission()
{
}

void
Mission::on_inactive()
{
	/* check anyway if missions have changed so that feedback to groundstation is given */
	bool onboard_updated;
	orb_check(_navigator->get_onboard_mission_sub(), &onboard_updated);
	if (onboard_updated) {
		update_onboard_mission();
	}

	bool offboard_updated;
	orb_check(_navigator->get_offboard_mission_sub(), &offboard_updated);
	if (offboard_updated) {
		update_offboard_mission();
	}

	if (!_navigator->get_can_loiter_at_sp() || _navigator->get_vstatus()->condition_landed) {
		_need_takeoff = true;
	}
}

void
Mission::on_activation()
{
	set_mission_items();
}

void
Mission::on_active()
{
	/* check if anything has changed */
	bool onboard_updated;
	orb_check(_navigator->get_onboard_mission_sub(), &onboard_updated);
	if (onboard_updated) {
		update_onboard_mission();
	}

	bool offboard_updated;
	orb_check(_navigator->get_offboard_mission_sub(), &offboard_updated);
	if (offboard_updated) {
		update_offboard_mission();
	}

	/* reset mission items if needed */
	if (onboard_updated || offboard_updated) {
		set_mission_items();
	}

	/* lets check if we reached the current mission item */
	if (_mission_type != MISSION_TYPE_NONE && is_mission_item_reached()) {
		advance_mission();
		set_mission_items();

	} else {
		/* if waypoint position reached allow loiter on the setpoint */
		if (_waypoint_position_reached && _mission_item.nav_cmd != NAV_CMD_IDLE) {
			_navigator->set_can_loiter_at_sp(true);
		}
	}
}

void
Mission::update_onboard_mission()
{
	if (orb_copy(ORB_ID(onboard_mission), _navigator->get_onboard_mission_sub(), &_onboard_mission) == OK) {
		/* accept the current index set by the onboard mission if it is within bounds */
		if (_onboard_mission.current_index >=0
		&& _onboard_mission.current_index < (int)_onboard_mission.count) {
			_current_onboard_mission_index = _onboard_mission.current_index;
		} else {
			/* if less WPs available, reset to first WP */
			if (_current_onboard_mission_index >= (int)_onboard_mission.count) {
				_current_onboard_mission_index = 0;
			/* if not initialized, set it to 0 */
			} else if (_current_onboard_mission_index < 0) {
				_current_onboard_mission_index = 0;
			}
			/* otherwise, just leave it */
		}

	} else {
		_onboard_mission.count = 0;
		_onboard_mission.current_index = 0;
		_current_onboard_mission_index = 0;
	}
}

void
Mission::update_offboard_mission()
{
	if (orb_copy(ORB_ID(offboard_mission), _navigator->get_offboard_mission_sub(), &_offboard_mission) == OK) {

		/* determine current index */
		if (_offboard_mission.current_index >= 0
		    && _offboard_mission.current_index < (int)_offboard_mission.count) {
			_current_offboard_mission_index = _offboard_mission.current_index;

		} else {
			/* if less WPs available, reset to first WP */
			if (_current_offboard_mission_index >= (int)_offboard_mission.count) {
				_current_offboard_mission_index = 0;

			/* if not initialized, set it to 0 */
			} else if (_current_offboard_mission_index < 0) {
				_current_offboard_mission_index = 0;
			}
			/* otherwise, just leave it */
		}

		/* Check mission feasibility, for now do not handle the return value,
		 * however warnings are issued to the gcs via mavlink from inside the MissionFeasiblityChecker */
		dm_item_t dm_current;

		if (_offboard_mission.dataman_id == 0) {
			dm_current = DM_KEY_WAYPOINTS_OFFBOARD_0;

		} else {
			dm_current = DM_KEY_WAYPOINTS_OFFBOARD_1;
		}

		missionFeasiblityChecker.checkMissionFeasible(_navigator->get_vstatus()->is_rotary_wing, dm_current,
			                                      (size_t)_offboard_mission.count,
							      _navigator->get_geofence(),
							      _navigator->get_home_position()->alt);

	} else {
		_offboard_mission.count = 0;
		_offboard_mission.current_index = 0;
		_current_offboard_mission_index = 0;
	}

	report_current_offboard_mission_item();
}


void
Mission::advance_mission()
{
	if (_takeoff) {
		_takeoff = false;

	} else {
		switch (_mission_type) {
		case MISSION_TYPE_ONBOARD:
			_current_onboard_mission_index++;
			break;

		case MISSION_TYPE_OFFBOARD:
			_current_offboard_mission_index++;
			break;

		case MISSION_TYPE_NONE:
		default:
			break;
		}
	}
}

void
Mission::set_mission_items()
{
	/* make sure param is up to date */
	updateParams();

	struct position_setpoint_triplet_s *pos_sp_triplet = _navigator->get_position_setpoint_triplet();

	/* set previous position setpoint to current */
	set_previous_pos_setpoint();

	/* try setting onboard mission item */
	if (_param_onboard_enabled.get() && read_mission_item(true, true, &_mission_item)) {
		/* if mission type changed, notify */
		if (_mission_type != MISSION_TYPE_ONBOARD) {
			mavlink_log_info(_navigator->get_mavlink_fd(), "#audio: onboard mission running");
		}
		_mission_type = MISSION_TYPE_ONBOARD;

	/* try setting offboard mission item */
	} else if (read_mission_item(false, true, &_mission_item)) {
		/* if mission type changed, notify */
		if (_mission_type != MISSION_TYPE_OFFBOARD) {
			mavlink_log_info(_navigator->get_mavlink_fd(), "#audio: offboard mission running");
		}
		_mission_type = MISSION_TYPE_OFFBOARD;

	} else {
		/* no mission available, switch to loiter */
		if (_mission_type != MISSION_TYPE_NONE) {
			mavlink_log_info(_navigator->get_mavlink_fd(),
					"#audio: mission finished");
		} else {
			mavlink_log_info(_navigator->get_mavlink_fd(),
					"#audio: no mission available");
		}
		_mission_type = MISSION_TYPE_NONE;

		/* set loiter mission item */
		set_loiter_item(&_mission_item);

		/* update position setpoint triplet  */
		pos_sp_triplet->previous.valid = false;
		mission_item_to_position_setpoint(&_mission_item, &pos_sp_triplet->current);
		pos_sp_triplet->next.valid = false;

		_navigator->set_can_loiter_at_sp(pos_sp_triplet->current.type == SETPOINT_TYPE_LOITER);

		reset_mission_item_reached();
		report_mission_finished();

		_navigator->set_position_setpoint_triplet_updated();
		return;
	}

	/* do takeoff on first waypoint for rotary wing vehicles */
	if (_navigator->get_vstatus()->is_rotary_wing) {
		/* force takeoff if landed (additional protection) */
		if (!_takeoff && _navigator->get_vstatus()->condition_landed) {
			_need_takeoff = true;
		}

		/* new current mission item set, check if we need takeoff */
		if (_need_takeoff && (
				_mission_item.nav_cmd == NAV_CMD_TAKEOFF ||
				_mission_item.nav_cmd == NAV_CMD_WAYPOINT ||
				_mission_item.nav_cmd == NAV_CMD_LOITER_TIME_LIMIT ||
				_mission_item.nav_cmd == NAV_CMD_LOITER_TURN_COUNT ||
				_mission_item.nav_cmd == NAV_CMD_LOITER_UNLIMITED ||
				_mission_item.nav_cmd == NAV_CMD_RETURN_TO_LAUNCH)) {
			_takeoff = true;
			_need_takeoff = false;
		}
	}

	if (_takeoff) {
		/* do takeoff before going to setpoint */
		/* set mission item as next position setpoint */
		mission_item_to_position_setpoint(&_mission_item, &pos_sp_triplet->next);

		/* calculate takeoff altitude */
		float takeoff_alt = _mission_item.altitude;
		if (_mission_item.altitude_is_relative) {
			takeoff_alt += _navigator->get_home_position()->alt;
		}

		/* perform takeoff at least to NAV_TAKEOFF_ALT above home/ground, even if first waypoint is lower */
		if (_navigator->get_vstatus()->condition_landed) {
			takeoff_alt = fmaxf(takeoff_alt, _navigator->get_global_position()->alt + _param_takeoff_alt.get());

		} else {
			takeoff_alt = fmaxf(takeoff_alt, _navigator->get_home_position()->alt + _param_takeoff_alt.get());
		}

		mavlink_log_info(_navigator->get_mavlink_fd(), "#audio: takeoff to %.1fm above home", (double)(takeoff_alt - _navigator->get_home_position()->alt));

		_mission_item.lat = _navigator->get_global_position()->lat;
		_mission_item.lon = _navigator->get_global_position()->lon;
		_mission_item.altitude = takeoff_alt;
		_mission_item.altitude_is_relative = false;

		mission_item_to_position_setpoint(&_mission_item, &pos_sp_triplet->current);

	} else {
		/* set current position setpoint from mission item */
		mission_item_to_position_setpoint(&_mission_item, &pos_sp_triplet->current);

		/* require takeoff after landing or idle */
		if (pos_sp_triplet->current.type == SETPOINT_TYPE_LAND || pos_sp_triplet->current.type == SETPOINT_TYPE_IDLE) {
			_need_takeoff = true;
		}

		_navigator->set_can_loiter_at_sp(false);
		reset_mission_item_reached();

		if (_mission_type == MISSION_TYPE_OFFBOARD) {
			report_current_offboard_mission_item();
		}
		// TODO: report onboard mission item somehow

		/* try to read next mission item */
		struct mission_item_s mission_item_next;

		if (read_mission_item(_mission_type == MISSION_TYPE_ONBOARD, false, &mission_item_next)) {
			/* got next mission item, update setpoint triplet */
			mission_item_to_position_setpoint(&mission_item_next, &pos_sp_triplet->next);

		} else {
			/* next mission item is not available */
			pos_sp_triplet->next.valid = false;
		}
	}

	_navigator->set_position_setpoint_triplet_updated();
}

bool
Mission::read_mission_item(bool onboard, bool is_current, struct mission_item_s *mission_item)
{
	/* select onboard/offboard mission */
	int *mission_index_ptr;
	struct mission_s *mission;
	dm_item_t dm_item;
	int mission_index_next;

	if (onboard) {
		/* onboard mission */
		mission_index_next = _current_onboard_mission_index + 1;
		mission_index_ptr = is_current ? &_current_onboard_mission_index : &mission_index_next;

		mission = &_onboard_mission;

		dm_item = DM_KEY_WAYPOINTS_ONBOARD;

	} else {
		/* offboard mission */
		mission_index_next = _current_offboard_mission_index + 1;
		mission_index_ptr = is_current ? &_current_offboard_mission_index : &mission_index_next;

		mission = &_offboard_mission;

		if (_offboard_mission.dataman_id == 0) {
			dm_item = DM_KEY_WAYPOINTS_OFFBOARD_0;

		} else {
			dm_item = DM_KEY_WAYPOINTS_OFFBOARD_1;
		}
	}

	if (*mission_index_ptr < 0 || *mission_index_ptr >= (int)mission->count) {
		/* mission item index out of bounds */
		return false;
	}

	/* repeat several to get the mission item because we might have to follow multiple DO_JUMPS */
	for (int i = 0; i < 10; i++) {
		const ssize_t len = sizeof(struct mission_item_s);

		/* read mission item to temp storage first to not overwrite current mission item if data damaged */
		struct mission_item_s mission_item_tmp;

		/* read mission item from datamanager */
		if (dm_read(dm_item, *mission_index_ptr, &mission_item_tmp, len) != len) {
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			mavlink_log_critical(_navigator->get_mavlink_fd(),
			                     "#audio: ERROR waypoint could not be read");
			return false;
		}

		/* check for DO_JUMP item, and whether it hasn't not already been repeated enough times */
		if (mission_item_tmp.nav_cmd == NAV_CMD_DO_JUMP) {

			/* do DO_JUMP as many times as requested */
			if (mission_item_tmp.do_jump_current_count < mission_item_tmp.do_jump_repeat_count) {

				/* only raise the repeat count if this is for the current mission item
				* but not for the next mission item */
				if (is_current) {
					(mission_item_tmp.do_jump_current_count)++;
					/* save repeat count */
					if (dm_write(dm_item, *mission_index_ptr, DM_PERSIST_IN_FLIGHT_RESET, &mission_item_tmp, len) != len) {
						/* not supposed to happen unless the datamanager can't access the
						 * dataman */
						mavlink_log_critical(_navigator->get_mavlink_fd(),
								"#audio: ERROR DO JUMP waypoint could not be written");
						return false;
					}
				}
				/* set new mission item index and repeat
				* we don't have to validate here, if it's invalid, we should realize this later .*/
				*mission_index_ptr = mission_item_tmp.do_jump_mission_index;

			} else {
				mavlink_log_info(_navigator->get_mavlink_fd(),
						 "#audio: DO JUMP repetitions completed");
				/* no more DO_JUMPS, therefore just try to continue with next mission item */
				(*mission_index_ptr)++;
			}

		} else {
			/* if it's not a DO_JUMP, then we were successful */
			memcpy(mission_item, &mission_item_tmp, sizeof(struct mission_item_s));
			return true;
		}
	}

	/* we have given up, we don't want to cycle forever */
	mavlink_log_critical(_navigator->get_mavlink_fd(),
			     "#audio: ERROR DO JUMP is cycling, giving up");
	return false;
}

void
Mission::report_mission_item_reached()
{
	if (_mission_type == MISSION_TYPE_OFFBOARD) {
		_mission_result.mission_reached = true;
		_mission_result.mission_index_reached = _current_offboard_mission_index;
	}
	publish_mission_result();
}

void
Mission::report_current_offboard_mission_item()
{
	_mission_result.index_current_mission = _current_offboard_mission_index;
	publish_mission_result();
}

void
Mission::report_mission_finished()
{
	_mission_result.mission_finished = true;
	publish_mission_result();
}

void
Mission::publish_mission_result()
{
	/* lazily publish the mission result only once available */
	if (_mission_result_pub > 0) {
		/* publish mission result */
		orb_publish(ORB_ID(mission_result), _mission_result_pub, &_mission_result);

	} else {
		/* advertise and publish */
		_mission_result_pub = orb_advertise(ORB_ID(mission_result), &_mission_result);
	}
	/* reset reached bool */
	_mission_result.mission_reached = false;
	_mission_result.mission_finished = false;
}
