/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file station_cmd.cpp Handling of station tiles. */

#include "stdafx.h"
#include "core/flatset_type.hpp"
#include "aircraft.h"
#include "bridge_map.h"
#include "vehiclelist_func.h"
#include "viewport_func.h"
#include "viewport_kdtree.h"
#include "command_func.h"
#include "town.h"
#include "news_func.h"
#include "train.h"
#include "ship.h"
#include "roadveh.h"
#include "industry.h"
#include "newgrf_cargo.h"
#include "newgrf_debug.h"
#include "newgrf_station.h"
#include "newgrf_canal.h" /* For the buoy */
#include "pathfinder/yapf/yapf_cache.h"
#include "road_internal.h" /* For drawing catenary/checking road removal */
#include "autoslope.h"
#include "water.h"
#include "strings_func.h"
#include "clear_func.h"
#include "timer/timer_game_calendar.h"
#include "vehicle_func.h"
#include "string_func.h"
#include "animated_tile_func.h"
#include "elrail_func.h"
#include "station_base.h"
#include "station_func.h"
#include "station_kdtree.h"
#include "roadstop_base.h"
#include "newgrf_railtype.h"
#include "newgrf_roadtype.h"
#include "waypoint_base.h"
#include "waypoint_func.h"
#include "pbs.h"
#include "debug.h"
#include "core/random_func.hpp"
#include "core/container_func.hpp"
#include "company_base.h"
#include "table/airporttile_ids.h"
#include "newgrf_airporttiles.h"
#include "order_backup.h"
#include "newgrf_house.h"
#include "company_gui.h"
#include "linkgraph/linkgraph_base.h"
#include "linkgraph/refresh.h"
#include "tunnelbridge_map.h"
#include "station_cmd.h"
#include "waypoint_cmd.h"
#include "landscape_cmd.h"
#include "rail_cmd.h"
#include "newgrf_roadstop.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "timer/timer_game_tick.h"
#include "cheat_type.h"
#include "road_func.h"

#include "widgets/station_widget.h"

#include "table/strings.h"

#include <bitset>

#include "safeguards.h"

/**
 * Static instance of FlowStat::SharesMap.
 * Note: This instance is created on task start.
 *       Lazy creation on first usage results in a data race between the CDist threads.
 */
/* static */ const FlowStat::SharesMap FlowStat::empty_sharesmap;

/**
 * Check whether the given tile is a hangar.
 * @param t the tile to of whether it is a hangar.
 * @pre IsTileType(t, MP_STATION)
 * @return true if and only if the tile is a hangar.
 */
bool IsHangar(Tile t)
{
	assert(IsTileType(t, MP_STATION));

	/* If the tile isn't an airport there's no chance it's a hangar. */
	if (!IsAirport(t)) return false;

	const Station *st = Station::GetByTile(t);
	const AirportSpec *as = st->airport.GetSpec();

	for (const auto &depot : as->depots) {
		if (st->airport.GetRotatedTileFromOffset(depot.ti) == TileIndex(t)) return true;
	}

	return false;
}

/**
 * Look for a station owned by the given company around the given tile area.
 * @param ta the area to search over
 * @param closest_station the closest owned station found so far
 * @param company the company whose stations to look for
 * @param st to 'return' the found station
 * @param filter Filter function
 * @return Succeeded command (if zero or one station found) or failed command (for two or more stations found).
 */
template <class T, class F>
CommandCost GetStationAround(TileArea ta, StationID closest_station, CompanyID company, T **st, F filter)
{
	ta.Expand(1);

	/* check around to see if there are any stations there owned by the company */
	for (TileIndex tile_cur : ta) {
		if (IsTileType(tile_cur, MP_STATION)) {
			StationID t = GetStationIndex(tile_cur);
			if (!T::IsValidID(t) || T::Get(t)->owner != company || !filter(T::Get(t))) continue;
			if (closest_station == StationID::Invalid()) {
				closest_station = t;
			} else if (closest_station != t) {
				return CommandCost(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
			}
		}
	}
	*st = (closest_station == StationID::Invalid()) ? nullptr : T::Get(closest_station);
	return CommandCost();
}

/**
 * Function to check whether the given tile matches some criterion.
 * @param tile the tile to check
 * @return true if it matches, false otherwise
 */
typedef bool (*CMSAMatcher)(TileIndex tile);

/**
 * Counts the numbers of tiles matching a specific type in the area around
 * @param tile the center tile of the 'count area'
 * @param cmp the comparator/matcher (@see CMSAMatcher)
 * @return the number of matching tiles around
 */
static int CountMapSquareAround(TileIndex tile, CMSAMatcher cmp)
{
	int num = 0;

	for (int dx = -3; dx <= 3; dx++) {
		for (int dy = -3; dy <= 3; dy++) {
			TileIndex t = TileAddWrap(tile, dx, dy);
			if (t != INVALID_TILE && cmp(t)) num++;
		}
	}

	return num;
}

/**
 * Check whether the tile is a mine.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a mine
 */
static bool CMSAMine(TileIndex tile)
{
	/* No industry */
	if (!IsTileType(tile, MP_INDUSTRY)) return false;

	const Industry *ind = Industry::GetByTile(tile);

	/* No extractive industry */
	if (!GetIndustrySpec(ind->type)->life_type.Test(IndustryLifeType::Extractive)) return false;

	for (const auto &p : ind->produced) {
		/* The industry extracts something non-liquid, i.e. no oil or plastic, so it is a mine.
		 * Also the production of passengers and mail is ignored. */
		if (IsValidCargoType(p.cargo) &&
				!CargoSpec::Get(p.cargo)->classes.Any({CargoClass::Liquid, CargoClass::Passengers, CargoClass::Mail})) {
			return true;
		}
	}

	return false;
}

/**
 * Check whether the tile is water.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a water tile
 */
static bool CMSAWater(TileIndex tile)
{
	return IsTileType(tile, MP_WATER) && IsWater(tile);
}

/**
 * Check whether the tile is a tree.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a tree tile
 */
static bool CMSATree(TileIndex tile)
{
	return IsTileType(tile, MP_TREES);
}

enum StationNaming : uint8_t {
	STATIONNAMING_RAIL,
	STATIONNAMING_ROAD,
	STATIONNAMING_AIRPORT,
	STATIONNAMING_OILRIG,
	STATIONNAMING_DOCK,
	STATIONNAMING_HELIPORT,
};

/** Information to handle station action 0 property 24 correctly */
struct StationNameInformation {
	std::bitset<STR_SV_STNAME_FALLBACK - STR_SV_STNAME> used_names; ///< Used default station suffixes.
	std::bitset<NUM_INDUSTRYTYPES> indtypes; ///< Bit set indicating when an industry type has been found.

	bool IsAvailable(StringID str) const
	{
		assert(IsInsideMM(str, STR_SV_STNAME, STR_SV_STNAME_FALLBACK));
		return !this->used_names.test(str - STR_SV_STNAME);
	}

	void SetUsed(StringID str)
	{
		assert(IsInsideMM(str, STR_SV_STNAME, STR_SV_STNAME_FALLBACK));
		this->used_names.set(str - STR_SV_STNAME);
	}
};

static StringID GenerateStationName(Station *st, TileIndex tile, StationNaming name_class)
{
	const Town *t = st->town;

	StationNameInformation sni{};

	for (const Station *s : Station::Iterate()) {
		if (s != st && s->town == t) {
			if (s->indtype != IT_INVALID) {
				sni.indtypes[s->indtype] = true;
				StringID name = GetIndustrySpec(s->indtype)->station_name;
				if (name != STR_UNDEFINED) {
					/* Filter for other industrytypes with the same name */
					for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
						const IndustrySpec *indsp = GetIndustrySpec(it);
						if (indsp->enabled && indsp->station_name == name) sni.indtypes[it] = true;
					}
				}
				continue;
			}
			if (IsInsideMM(s->string_id, STR_SV_STNAME, STR_SV_STNAME_FALLBACK)) {
				auto str = s->string_id;
				if (str == STR_SV_STNAME_FOREST) str = STR_SV_STNAME_WOODS;
				sni.SetUsed(str);
			}
		}
	}

	for (auto indtile : SpiralTileSequence(tile, 7)) {
		if (!IsTileType(indtile, MP_INDUSTRY)) continue;

		/* If the station name is undefined it means that it doesn't name a station */
		IndustryType indtype = GetIndustryType(indtile);
		const IndustrySpec *indsp = GetIndustrySpec(indtype);
		if (indsp->station_name == STR_UNDEFINED) continue;

		/* In all cases if an industry that provides a name is found two of
		 * the standard names will be disabled. */
		sni.SetUsed(STR_SV_STNAME_OILFIELD);
		sni.SetUsed(STR_SV_STNAME_MINES);
		if (sni.indtypes[indtype]) continue;

		/* STR_NULL means it only disables oil rig/mines */
		if (indsp->station_name != STR_NULL) {
			st->indtype = indtype;
			return STR_SV_STNAME_FALLBACK;
		}
		break;
	}

	/* check default names
	 * Oil rigs/mines name could be marked not free by looking for a near by industry. */
	switch (name_class) {
		case STATIONNAMING_AIRPORT:
			if (sni.IsAvailable(STR_SV_STNAME_AIRPORT)) return STR_SV_STNAME_AIRPORT;
			break;
		case STATIONNAMING_OILRIG:
			if (sni.IsAvailable(STR_SV_STNAME_OILFIELD)) return STR_SV_STNAME_OILFIELD;
			break;
		case STATIONNAMING_DOCK:
			if (sni.IsAvailable(STR_SV_STNAME_DOCKS)) return STR_SV_STNAME_DOCKS;
			break;
		case STATIONNAMING_HELIPORT:
			if (sni.IsAvailable(STR_SV_STNAME_HELIPORT)) return STR_SV_STNAME_HELIPORT;
			break;
		default:
			break;
	};

	/* check mine? */
	if (sni.IsAvailable(STR_SV_STNAME_MINES)) {
		if (CountMapSquareAround(tile, CMSAMine) >= 2) {
			return STR_SV_STNAME_MINES;
		}
	}

	/* check close enough to town to get central as name? */
	if (DistanceMax(tile, t->xy) < 8) {
		if (sni.IsAvailable(STR_SV_STNAME)) return STR_SV_STNAME;
		if (sni.IsAvailable(STR_SV_STNAME_CENTRAL)) return STR_SV_STNAME_CENTRAL;
	}

	/* Check lakeside */
	if (sni.IsAvailable(STR_SV_STNAME_LAKESIDE) &&
			DistanceFromEdge(tile) < 20 &&
			CountMapSquareAround(tile, CMSAWater) >= 5) {
		return STR_SV_STNAME_LAKESIDE;
	}

	/* Check woods */
	if (sni.IsAvailable(STR_SV_STNAME_WOODS) && (
				CountMapSquareAround(tile, CMSATree) >= 8 ||
				CountMapSquareAround(tile, IsTileForestIndustry) >= 2)
			) {
		return _settings_game.game_creation.landscape == LandscapeType::Tropic ? STR_SV_STNAME_FOREST : STR_SV_STNAME_WOODS;
	}

	/* check elevation compared to town */
	int z = GetTileZ(tile);
	int z2 = GetTileZ(t->xy);
	if (z < z2) {
		if (sni.IsAvailable(STR_SV_STNAME_VALLEY)) return STR_SV_STNAME_VALLEY;
	} else if (z > z2) {
		if (sni.IsAvailable(STR_SV_STNAME_HEIGHTS)) return STR_SV_STNAME_HEIGHTS;
	}

	/* check direction compared to town */
	if (TileX(tile) < TileX(t->xy)) {
		sni.SetUsed(STR_SV_STNAME_SOUTH);
		sni.SetUsed(STR_SV_STNAME_WEST);
	} else {
		sni.SetUsed(STR_SV_STNAME_NORTH);
		sni.SetUsed(STR_SV_STNAME_EAST);
	}
	if (TileY(tile) < TileY(t->xy)) {
		sni.SetUsed(STR_SV_STNAME_SOUTH);
		sni.SetUsed(STR_SV_STNAME_EAST);
	} else {
		sni.SetUsed(STR_SV_STNAME_NORTH);
		sni.SetUsed(STR_SV_STNAME_WEST);
	}

	/** Remaining station names that can be used when a more specific name has not been used. */
	static const StringID fallback_names[] = {
		STR_SV_STNAME_NORTH,
		STR_SV_STNAME_SOUTH,
		STR_SV_STNAME_EAST,
		STR_SV_STNAME_WEST,
		STR_SV_STNAME_TRANSFER,
		STR_SV_STNAME_HALT,
		STR_SV_STNAME_EXCHANGE,
		STR_SV_STNAME_ANNEXE,
		STR_SV_STNAME_SIDINGS,
		STR_SV_STNAME_BRANCH,
		STR_SV_STNAME_UPPER,
		STR_SV_STNAME_LOWER,
	};
	for (auto str : fallback_names) {
		if (sni.IsAvailable(str)) return str;
	}
	return STR_SV_STNAME_FALLBACK;
}

/**
 * Find the closest deleted station of the current company
 * @param tile the tile to search from.
 * @return the closest station or nullptr if too far.
 */
static Station *GetClosestDeletedStation(TileIndex tile)
{
	uint threshold = 8;

	Station *best_station = nullptr;
	ForAllStationsRadius(tile, threshold, [&](Station *st) {
		if (!st->IsInUse() && st->owner == _current_company) {
			uint cur_dist = DistanceManhattan(tile, st->xy);

			if (cur_dist < threshold) {
				threshold = cur_dist;
				best_station = st;
			} else if (cur_dist == threshold && best_station != nullptr) {
				/* In case of a tie, lowest station ID wins */
				if (st->index < best_station->index) best_station = st;
			}
		}
	});

	return best_station;
}


void Station::GetTileArea(TileArea *ta, StationType type) const
{
	switch (type) {
		case StationType::Rail:
			*ta = this->train_station;
			return;

		case StationType::Airport:
			*ta = this->airport;
			return;

		case StationType::Truck:
			*ta = this->truck_station;
			return;

		case StationType::Bus:
			*ta = this->bus_station;
			return;

		case StationType::Dock:
		case StationType::Oilrig:
			*ta = this->docking_station;
			return;

		default: NOT_REACHED();
	}
}

/**
 * Update the virtual coords needed to draw the station sign.
 */
void Station::UpdateVirtCoord()
{
	Point pt = RemapCoords2(TileX(this->xy) * TILE_SIZE, TileY(this->xy) * TILE_SIZE);

	pt.y -= 32 * ZOOM_BASE;
	if (this->facilities.Test(StationFacility::Airport) && this->airport.type == AT_OILRIG) pt.y -= 16 * ZOOM_BASE;

	if (this->sign.kdtree_valid) _viewport_sign_kdtree.Remove(ViewportSignKdtreeItem::MakeStation(this->index));

	this->sign.UpdatePosition(pt.x, pt.y, GetString(STR_VIEWPORT_STATION, this->index, this->facilities), GetString(STR_STATION_NAME, this->index, this->facilities));

	_viewport_sign_kdtree.Insert(ViewportSignKdtreeItem::MakeStation(this->index));

	SetWindowDirty(WC_STATION_VIEW, this->index);
}

/**
 * Move the station main coordinate somewhere else.
 * @param new_xy new tile location of the sign
 */
void Station::MoveSign(TileIndex new_xy)
{
	if (this->xy == new_xy) return;

	_station_kdtree.Remove(this->index);

	this->BaseStation::MoveSign(new_xy);

	_station_kdtree.Insert(this->index);
}

/** Update the virtual coords needed to draw the station sign for all stations. */
void UpdateAllStationVirtCoords()
{
	for (BaseStation *st : BaseStation::Iterate()) {
		st->UpdateVirtCoord();
	}
}

void BaseStation::FillCachedName() const
{
	auto tmp_params = MakeParameters(this->index);
	this->cached_name = GetStringWithArgs(Waypoint::IsExpected(this) ? STR_WAYPOINT_NAME : STR_STATION_NAME, tmp_params);
}

void ClearAllStationCachedNames()
{
	for (BaseStation *st : BaseStation::Iterate()) {
		st->cached_name.clear();
	}
}

/**
 * Get a mask of the cargo types that the station accepts.
 * @param st Station to query
 * @return the expected mask
 */
CargoTypes GetAcceptanceMask(const Station *st)
{
	CargoTypes mask = 0;

	for (auto it = std::begin(st->goods); it != std::end(st->goods); ++it) {
		if (it->status.Test(GoodsEntry::State::Acceptance)) SetBit(mask, std::distance(std::begin(st->goods), it));
	}
	return mask;
}

/**
 * Get a mask of the cargo types that are empty at the station.
 * @param st Station to query
 * @return the empty mask
 */
CargoTypes GetEmptyMask(const Station *st)
{
	CargoTypes mask = 0;

	for (auto it = std::begin(st->goods); it != std::end(st->goods); ++it) {
		if (!it->HasData() || it->GetData().cargo.TotalCount() == 0) SetBit(mask, std::distance(std::begin(st->goods), it));
	}
	return mask;
}

/**
 * Add news item for when a station changes which cargoes it accepts.
 * @param st Station of cargo change.
 * @param cargoes Bit mask of cargo types to list.
 * @param reject True iff the station rejects the cargo types.
 */
static void ShowRejectOrAcceptNews(const Station *st, CargoTypes cargoes, bool reject)
{
	StringID msg = reject ? STR_NEWS_STATION_NO_LONGER_ACCEPTS_CARGO_LIST : STR_NEWS_STATION_NOW_ACCEPTS_CARGO_LIST;
	AddNewsItem(GetEncodedString(msg, st->index, cargoes), NewsType::Acceptance, NewsStyle::Small, NewsFlag::InColour, st->index);
}

/**
 * Get the cargo types being produced around the tile (in a rectangle).
 * @param north_tile Northern most tile of area
 * @param w X extent of the area
 * @param h Y extent of the area
 * @param rad Search radius in addition to the given area
 */
CargoArray GetProductionAroundTiles(TileIndex north_tile, int w, int h, int rad)
{
	CargoArray produced{};
	FlatSet<IndustryID> industries;
	TileArea ta = TileArea(north_tile, w, h).Expand(rad);

	/* Loop over all tiles to get the produced cargo of
	 * everything except industries */
	for (TileIndex tile : ta) {
		if (IsTileType(tile, MP_INDUSTRY)) industries.insert(GetIndustryIndex(tile));
		AddProducedCargo(tile, produced);
	}

	/* Loop over the seen industries. They produce cargo for
	 * anything that is within 'rad' of any one of their tiles.
	 */
	for (IndustryID industry : industries) {
		const Industry *i = Industry::Get(industry);
		/* Skip industry with neutral station */
		if (i->neutral_station != nullptr && !_settings_game.station.serve_neutral_industries) continue;

		for (const auto &p : i->produced) {
			if (IsValidCargoType(p.cargo)) produced[p.cargo]++;
		}
	}

	return produced;
}

/**
 * Get the acceptance of cargoes around the tile in 1/8.
 * @param center_tile Center of the search area
 * @param w X extent of area
 * @param h Y extent of area
 * @param rad Search radius in addition to given area
 * @param always_accepted bitmask of cargo accepted by houses and headquarters; can be nullptr
 * @param ind Industry associated with neutral station (e.g. oil rig) or nullptr
 */
CargoArray GetAcceptanceAroundTiles(TileIndex center_tile, int w, int h, int rad, CargoTypes *always_accepted)
{
	CargoArray acceptance{};
	if (always_accepted != nullptr) *always_accepted = 0;

	TileArea ta = TileArea(center_tile, w, h).Expand(rad);

	for (TileIndex tile : ta) {
		/* Ignore industry if it has a neutral station. */
		if (!_settings_game.station.serve_neutral_industries && IsTileType(tile, MP_INDUSTRY) && Industry::GetByTile(tile)->neutral_station != nullptr) continue;

		AddAcceptedCargo(tile, acceptance, always_accepted);
	}

	return acceptance;
}

/**
 * Get the acceptance of cargoes around the station in.
 * @param st Station to get acceptance of.
 * @param always_accepted bitmask of cargo accepted by houses and headquarters; can be nullptr
 */
static CargoArray GetAcceptanceAroundStation(const Station *st, CargoTypes *always_accepted)
{
	CargoArray acceptance{};
	if (always_accepted != nullptr) *always_accepted = 0;

	BitmapTileIterator it(st->catchment_tiles);
	for (TileIndex tile = it; tile != INVALID_TILE; tile = ++it) {
		AddAcceptedCargo(tile, acceptance, always_accepted);
	}

	return acceptance;
}

/**
 * Update the acceptance for a station.
 * @param st Station to update
 * @param show_msg controls whether to display a message that acceptance was changed.
 */
void UpdateStationAcceptance(Station *st, bool show_msg)
{
	/* old accepted goods types */
	CargoTypes old_acc = GetAcceptanceMask(st);

	/* And retrieve the acceptance. */
	CargoArray acceptance{};
	if (!st->rect.IsEmpty()) {
		acceptance = GetAcceptanceAroundStation(st, &st->always_accepted);
	}

	/* Adjust in case our station only accepts fewer kinds of goods */
	for (CargoType cargo = 0; cargo < NUM_CARGO; ++cargo) {
		uint amt = acceptance[cargo];

		/* Make sure the station can accept the goods type. */
		bool is_passengers = IsCargoInClass(cargo, CargoClass::Passengers);
		if ((!is_passengers && !st->facilities.Any({StationFacility::Train, StationFacility::TruckStop, StationFacility::Airport, StationFacility::Dock})) ||
				(is_passengers && !st->facilities.Any({StationFacility::Train, StationFacility::BusStop, StationFacility::Airport, StationFacility::Dock}))) {
			amt = 0;
		}

		GoodsEntry &ge = st->goods[cargo];
		ge.status.Set(GoodsEntry::State::Acceptance, amt >= 8);
		if (LinkGraph::IsValidID(ge.link_graph)) {
			(*LinkGraph::Get(ge.link_graph))[ge.node].SetDemand(amt / 8);
		}
	}

	/* Only show a message in case the acceptance was actually changed. */
	CargoTypes new_acc = GetAcceptanceMask(st);
	if (old_acc == new_acc) return;

	/* show a message to report that the acceptance was changed? */
	if (show_msg && st->owner == _local_company && st->IsInUse()) {
		/* Combine old and new masks to get changes */
		CargoTypes accepts = new_acc & ~old_acc;
		CargoTypes rejects = ~new_acc & old_acc;

		/* Show news message if there are any changes */
		if (accepts != 0) ShowRejectOrAcceptNews(st, accepts, false);
		if (rejects != 0) ShowRejectOrAcceptNews(st, rejects, true);
	}

	/* redraw the station view since acceptance changed */
	SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_ACCEPT_RATING_LIST);
}

static void UpdateStationSignCoord(BaseStation *st)
{
	const StationRect *r = &st->rect;

	if (r->IsEmpty()) return; // no tiles belong to this station

	/* clamp sign coord to be inside the station rect */
	TileIndex new_xy = TileXY(ClampU(TileX(st->xy), r->left, r->right), ClampU(TileY(st->xy), r->top, r->bottom));
	st->MoveSign(new_xy);

	if (!Station::IsExpected(st)) return;
	Station *full_station = Station::From(st);
	for (const GoodsEntry &ge : full_station->goods) {
		LinkGraphID lg = ge.link_graph;
		if (!LinkGraph::IsValidID(lg)) continue;
		(*LinkGraph::Get(lg))[ge.node].UpdateLocation(st->xy);
	}
}

/**
 * Common part of building various station parts and possibly attaching them to an existing one.
 * @param[in,out] st Station to attach to
 * @param flags Command flags
 * @param reuse Whether to try to reuse a deleted station (gray sign) if possible
 * @param area Area occupied by the new part
 * @param name_class Station naming class to use to generate the new station's name
 * @return Command error that occurred, if any
 */
static CommandCost BuildStationPart(Station **st, DoCommandFlags flags, bool reuse, TileArea area, StationNaming name_class)
{
	/* Find a deleted station close to us */
	if (*st == nullptr && reuse) *st = GetClosestDeletedStation(area.tile);

	if (*st != nullptr) {
		if ((*st)->owner != _current_company) {
			return CommandCost(CMD_ERROR);
		}

		CommandCost ret = (*st)->rect.BeforeAddRect(area.tile, area.w, area.h, StationRect::ADD_TEST);
		if (ret.Failed()) return ret;
	} else {
		/* allocate and initialize new station */
		if (!Station::CanAllocateItem()) return CommandCost(STR_ERROR_TOO_MANY_STATIONS_LOADING);

		if (flags.Test(DoCommandFlag::Execute)) {
			*st = new Station(area.tile);
			_station_kdtree.Insert((*st)->index);

			(*st)->town = ClosestTownFromTile(area.tile, UINT_MAX);
			(*st)->string_id = GenerateStationName(*st, area.tile, name_class);

			if (Company::IsValidID(_current_company)) {
				(*st)->town->have_ratings.Set(_current_company);
			}
		}
	}
	return CommandCost();
}

/**
 * This is called right after a station was deleted.
 * It checks if the whole station is free of substations, and if so, the station will be
 * deleted after a little while.
 * @param st Station
 */
static void DeleteStationIfEmpty(BaseStation *st)
{
	if (!st->IsInUse()) {
		st->delete_ctr = 0;
		InvalidateWindowData(WC_STATION_LIST, st->owner, 0);
	}
	/* station remains but it probably lost some parts - station sign should stay in the station boundaries */
	UpdateStationSignCoord(st);
}

/**
 * After adding/removing tiles to station, update some station-related stuff.
 * @param adding True if adding tiles, false if removing them.
 * @param type StationType being modified.
 */
void Station::AfterStationTileSetChange(bool adding, StationType type)
{
	this->UpdateVirtCoord();
	DirtyCompanyInfrastructureWindows(this->owner);

	if (adding) {
		this->RecomputeCatchment();
		MarkCatchmentTilesDirty();
		InvalidateWindowData(WC_STATION_LIST, this->owner, 0);
	} else {
		MarkCatchmentTilesDirty();
	}

	switch (type) {
		case StationType::Rail:
			SetWindowWidgetDirty(WC_STATION_VIEW, this->index, WID_SV_TRAINS);
			break;
		case StationType::Airport:
			break;
		case StationType::Truck:
		case StationType::Bus:
			SetWindowWidgetDirty(WC_STATION_VIEW, this->index, WID_SV_ROADVEHS);
			break;
		case StationType::Dock:
			SetWindowWidgetDirty(WC_STATION_VIEW, this->index, WID_SV_SHIPS);
			break;
		default: NOT_REACHED();
	}

	if (adding) {
		UpdateStationAcceptance(this, false);
		InvalidateWindowData(WC_SELECT_STATION, 0, 0);
	} else {
		DeleteStationIfEmpty(this);
		this->RecomputeCatchment();
	}

}

CommandCost ClearTile_Station(TileIndex tile, DoCommandFlags flags);

/**
 * Checks if the given tile is buildable, flat and has a certain height.
 * @param tile TileIndex to check.
 * @param invalid_dirs Prohibited directions for slopes (set of #DiagDirection).
 * @param allowed_z Height allowed for the tile. If allowed_z is negative, it will be set to the height of this tile.
 * @param allow_steep Whether steep slopes are allowed.
 * @param check_bridge Check for the existence of a bridge.
 * @return The cost in case of success, or an error code if it failed.
 */
CommandCost CheckBuildableTile(TileIndex tile, DiagDirections invalid_dirs, int &allowed_z, bool allow_steep, bool check_bridge = true)
{
	if (check_bridge && IsBridgeAbove(tile)) {
		return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);
	}

	CommandCost ret = EnsureNoVehicleOnGround(tile);
	if (ret.Failed()) return ret;

	auto [tileh, z] = GetTileSlopeZ(tile);

	/* Prohibit building if
	 *   1) The tile is "steep" (i.e. stretches two height levels).
	 *   2) The tile is non-flat and the build_on_slopes switch is disabled.
	 */
	if ((!allow_steep && IsSteepSlope(tileh)) ||
			((!_settings_game.construction.build_on_slopes) && tileh != SLOPE_FLAT)) {
		return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
	}

	CommandCost cost(EXPENSES_CONSTRUCTION);
	int flat_z = z + GetSlopeMaxZ(tileh);
	if (tileh != SLOPE_FLAT) {
		/* Forbid building if the tile faces a slope in a invalid direction. */
		for (DiagDirection dir = DIAGDIR_BEGIN; dir != DIAGDIR_END; dir++) {
			if (invalid_dirs.Test(dir) && !CanBuildDepotByTileh(dir, tileh)) {
				return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
			}
		}
		cost.AddCost(_price[PR_BUILD_FOUNDATION]);
	}

	/* The level of this tile must be equal to allowed_z. */
	if (allowed_z < 0) {
		/* First tile. */
		allowed_z = flat_z;
	} else if (allowed_z != flat_z) {
		return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
	}

	return cost;
}

/**
 * Checks if an airport can be built at the given location and clear the area.
 * @param tile_iter Airport tile iterator.
 * @param flags Operation to perform.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CheckFlatLandAirport(AirportTileTableIterator tile_iter, DoCommandFlags flags)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	int allowed_z = -1;

	for (; tile_iter != INVALID_TILE; ++tile_iter) {
		CommandCost ret = CheckBuildableTile(tile_iter, {}, allowed_z, true);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());

		ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile_iter);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());
	}

	return cost;
}

/**
 * Checks if a rail station can be built at the given tile.
 * @param tile_cur Tile to check.
 * @param north_tile North tile of the area being checked.
 * @param allowed_z Height allowed for the tile. If allowed_z is negative, it will be set to the height of this tile.
 * @param flags Operation to perform.
 * @param axis Rail station axis.
 * @param station StationID to be queried and returned if available.
 * @param rt The rail type to check for (overbuilding rail stations over rail).
 * @param affected_vehicles List of trains with PBS reservations on the tiles
 * @param spec_class Station class.
 * @param spec_index Index into the station class.
 * @param plat_len Platform length.
 * @param numtracks Number of platforms.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CheckFlatLandRailStation(TileIndex tile_cur, TileIndex north_tile, int &allowed_z, DoCommandFlags flags, Axis axis, StationID *station, RailType rt, std::vector<Train *> &affected_vehicles, StationClassID spec_class, uint16_t spec_index, uint8_t plat_len, uint8_t numtracks)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	DiagDirections invalid_dirs = AxisToDiagDirs(axis);

	const StationSpec *statspec = StationClass::Get(spec_class)->GetSpec(spec_index);
	bool slope_cb = statspec != nullptr && statspec->callback_mask.Test(StationCallbackMask::SlopeCheck);

	CommandCost ret = CheckBuildableTile(tile_cur, invalid_dirs, allowed_z, false);
	if (ret.Failed()) return ret;
	cost.AddCost(ret.GetCost());

	if (slope_cb) {
		/* Do slope check if requested. */
		ret = PerformStationTileSlopeCheck(north_tile, tile_cur, statspec, axis, plat_len, numtracks);
		if (ret.Failed()) return ret;
	}

	/* if station is set, then we have special handling to allow building on top of already existing stations.
	 * so station points to StationID::Invalid() if we can build on any station.
	 * Or it points to a station if we're only allowed to build on exactly that station. */
	if (station != nullptr && IsTileType(tile_cur, MP_STATION)) {
		if (!IsRailStation(tile_cur)) {
			return ClearTile_Station(tile_cur, DoCommandFlag::Auto); // get error message
		} else {
			StationID st = GetStationIndex(tile_cur);
			if (*station == StationID::Invalid()) {
				*station = st;
			} else if (*station != st) {
				return CommandCost(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
			}
		}
	} else {
		/* If we are building a station with a valid railtype, we may be able to overbuild an existing rail tile. */
		if (rt != INVALID_RAILTYPE && IsPlainRailTile(tile_cur)) {
			/* Don't overbuild signals. */
			if (HasSignals(tile_cur)) return CommandCost(STR_ERROR_MUST_REMOVE_SIGNALS_FIRST);

			/* The current rail type must have power on the to-be-built type (e.g. convert normal rail to electrified rail). */
			if (HasPowerOnRail(GetRailType(tile_cur), rt)) {
				TrackBits tracks = GetTrackBits(tile_cur);
				Track track = RemoveFirstTrack(&tracks);
				Track expected_track = invalid_dirs.Test(DIAGDIR_NE) ? TRACK_X : TRACK_Y;

				/* The existing track must align with the desired station axis. */
				if (tracks == TRACK_BIT_NONE && track == expected_track) {
					/* Check for trains having a reservation for this tile. */
					if (HasBit(GetRailReservationTrackBits(tile_cur), track)) {
						Train *v = GetTrainForReservation(tile_cur, track);
						if (v != nullptr) {
							affected_vehicles.push_back(v);
						}
					}
					ret = Command<CMD_REMOVE_SINGLE_RAIL>::Do(flags, tile_cur, track);
					if (ret.Failed()) return ret;
					cost.AddCost(ret.GetCost());
					/* With DoCommandFlags{flags}.Reset(DoCommandFlag::Execute) CmdLandscapeClear would fail since the rail still exists */
					return cost;
				}
			}
		}
		ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile_cur);
		if (ret.Failed()) return ret;
		cost.AddCost(ret.GetCost());
	}

	return cost;
}

/**
 * Checks if a road stop can be built at the given tile.
 * @param cur_tile Tile to check.
 * @param allowed_z Height allowed for the tile. If allowed_z is negative, it will be set to the height of this tile.
 * @param flags Operation to perform.
 * @param invalid_dirs Prohibited directions (set of DiagDirections).
 * @param is_drive_through True if trying to build a drive-through station.
 * @param station_type Station type (bus, truck or road waypoint).
 * @param axis Axis of a drive-through road stop.
 * @param station StationID to be queried and returned if available.
 * @param rt Road type to build, may be INVALID_ROADTYPE if an existing road is required.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CheckFlatLandRoadStop(TileIndex cur_tile, int &allowed_z, DoCommandFlags flags, DiagDirections invalid_dirs, bool is_drive_through, StationType station_type, Axis axis, StationID *station, RoadType rt)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);

	CommandCost ret = CheckBuildableTile(cur_tile, invalid_dirs, allowed_z, !is_drive_through);
	if (ret.Failed()) return ret;
	cost.AddCost(ret.GetCost());

	/* If station is set, then we have special handling to allow building on top of already existing stations.
	 * Station points to StationID::Invalid() if we can build on any station.
	 * Or it points to a station if we're only allowed to build on exactly that station. */
	if (station != nullptr && IsTileType(cur_tile, MP_STATION)) {
		if (!IsAnyRoadStop(cur_tile)) {
			return ClearTile_Station(cur_tile, DoCommandFlag::Auto); // Get error message.
		} else {
			if (station_type != GetStationType(cur_tile) ||
					is_drive_through != IsDriveThroughStopTile(cur_tile)) {
				return ClearTile_Station(cur_tile, DoCommandFlag::Auto); // Get error message.
			}
			/* Drive-through station in the wrong direction. */
			if (is_drive_through && IsDriveThroughStopTile(cur_tile) && GetDriveThroughStopAxis(cur_tile) != axis) {
				return CommandCost(STR_ERROR_DRIVE_THROUGH_DIRECTION);
			}
			StationID st = GetStationIndex(cur_tile);
			if (*station == StationID::Invalid()) {
				*station = st;
			} else if (*station != st) {
				return CommandCost(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
			}
		}
	} else {
		bool build_over_road = is_drive_through && IsNormalRoadTile(cur_tile);
		/* Road bits in the wrong direction. */
		RoadBits rb = IsNormalRoadTile(cur_tile) ? GetAllRoadBits(cur_tile) : ROAD_NONE;
		if (build_over_road && (rb & (axis == AXIS_X ? ROAD_Y : ROAD_X)) != 0) {
			/* Someone was pedantic and *NEEDED* three fracking different error messages. */
			switch (CountBits(rb)) {
				case 1:
					return CommandCost(STR_ERROR_DRIVE_THROUGH_DIRECTION);

				case 2:
					if (rb == ROAD_X || rb == ROAD_Y) return CommandCost(STR_ERROR_DRIVE_THROUGH_DIRECTION);
					return CommandCost(STR_ERROR_DRIVE_THROUGH_CORNER);

				default: // 3 or 4
					return CommandCost(STR_ERROR_DRIVE_THROUGH_JUNCTION);
			}
		}

		if (build_over_road) {
			/* There is a road, check if we can build road+tram stop over it. */
			RoadType road_rt = GetRoadType(cur_tile, RTT_ROAD);
			if (road_rt != INVALID_ROADTYPE) {
				Owner road_owner = GetRoadOwner(cur_tile, RTT_ROAD);
				if (road_owner == OWNER_TOWN) {
					if (!_settings_game.construction.road_stop_on_town_road) return CommandCost(STR_ERROR_DRIVE_THROUGH_ON_TOWN_ROAD);
				} else if (!_settings_game.construction.road_stop_on_competitor_road && road_owner != OWNER_NONE) {
					ret = CheckOwnership(road_owner);
					if (ret.Failed()) return ret;
				}
				uint num_pieces = CountBits(GetRoadBits(cur_tile, RTT_ROAD));

				if (rt != INVALID_ROADTYPE && RoadTypeIsRoad(rt) && !HasPowerOnRoad(rt, road_rt)) return CommandCost(STR_ERROR_NO_SUITABLE_ROAD);

				if (GetDisallowedRoadDirections(cur_tile) != DRD_NONE && road_owner != OWNER_TOWN) {
					ret = CheckOwnership(road_owner);
					if (ret.Failed()) return ret;
				}

				cost.AddCost(RoadBuildCost(road_rt) * (2 - num_pieces));
			} else if (rt != INVALID_ROADTYPE && RoadTypeIsRoad(rt)) {
				cost.AddCost(RoadBuildCost(rt) * 2);
			}

			/* There is a tram, check if we can build road+tram stop over it. */
			RoadType tram_rt = GetRoadType(cur_tile, RTT_TRAM);
			if (tram_rt != INVALID_ROADTYPE) {
				Owner tram_owner = GetRoadOwner(cur_tile, RTT_TRAM);
				if (Company::IsValidID(tram_owner) &&
						(!_settings_game.construction.road_stop_on_competitor_road ||
						/* Disallow breaking end-of-line of someone else
						 * so trams can still reverse on this tile. */
							HasExactlyOneBit(GetRoadBits(cur_tile, RTT_TRAM)))) {
					ret = CheckOwnership(tram_owner);
					if (ret.Failed()) return ret;
				}
				uint num_pieces = CountBits(GetRoadBits(cur_tile, RTT_TRAM));

				if (rt != INVALID_ROADTYPE && RoadTypeIsTram(rt) && !HasPowerOnRoad(rt, tram_rt)) return CommandCost(STR_ERROR_NO_SUITABLE_ROAD);

				cost.AddCost(RoadBuildCost(tram_rt) * (2 - num_pieces));
			} else if (rt != INVALID_ROADTYPE && RoadTypeIsTram(rt)) {
				cost.AddCost(RoadBuildCost(rt) * 2);
			}
		} else if (rt == INVALID_ROADTYPE) {
			return CommandCost(STR_ERROR_THERE_IS_NO_ROAD);
		} else {
			ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, cur_tile);
			if (ret.Failed()) return ret;
			cost.AddCost(ret.GetCost());
			cost.AddCost(RoadBuildCost(rt) * 2);
		}
	}

	return cost;
}

/**
 * Check whether we can expand the rail part of the given station.
 * @param st the station to expand
 * @param new_ta the current (and if all is fine new) tile area of the rail part of the station
 * @return Succeeded or failed command.
 */
CommandCost CanExpandRailStation(const BaseStation *st, TileArea &new_ta)
{
	TileArea cur_ta = st->train_station;

	/* determine new size of train station region.. */
	int x = std::min(TileX(cur_ta.tile), TileX(new_ta.tile));
	int y = std::min(TileY(cur_ta.tile), TileY(new_ta.tile));
	new_ta.w = std::max(TileX(cur_ta.tile) + cur_ta.w, TileX(new_ta.tile) + new_ta.w) - x;
	new_ta.h = std::max(TileY(cur_ta.tile) + cur_ta.h, TileY(new_ta.tile) + new_ta.h) - y;
	new_ta.tile = TileXY(x, y);

	/* make sure the final size is not too big. */
	if (new_ta.w > _settings_game.station.station_spread || new_ta.h > _settings_game.station.station_spread) {
		return CommandCost(STR_ERROR_STATION_TOO_SPREAD_OUT);
	}

	return CommandCost();
}

static inline uint8_t *CreateSingle(uint8_t *layout, int n)
{
	int i = n;
	do *layout++ = 0; while (--i);
	layout[((n - 1) >> 1) - n] = 2;
	return layout;
}

static inline uint8_t *CreateMulti(uint8_t *layout, int n, uint8_t b)
{
	int i = n;
	do *layout++ = b; while (--i);
	if (n > 4) {
		layout[0 - n] = 0;
		layout[n - 1 - n] = 0;
	}
	return layout;
}

/**
 * Create the station layout for the given number of tracks and platform length.
 * @param layout    The layout to write to.
 * @param numtracks The number of tracks to write.
 * @param plat_len  The length of the platforms.
 * @param statspec  The specification of the station to (possibly) get the layout from.
 */
void GetStationLayout(uint8_t *layout, uint numtracks, uint plat_len, const StationSpec *statspec)
{
	if (statspec != nullptr) {
		auto found = statspec->layouts.find(GetStationLayoutKey(numtracks, plat_len));
		if (found != std::end(statspec->layouts)) {
			/* Custom layout defined, copy to buffer. */
			std::copy(std::begin(found->second), std::end(found->second), layout);
			return;
		}
	}

	if (plat_len == 1) {
		CreateSingle(layout, numtracks);
	} else {
		if (numtracks & 1) layout = CreateSingle(layout, plat_len);
		int n = numtracks >> 1;

		while (--n >= 0) {
			layout = CreateMulti(layout, plat_len, 4);
			layout = CreateMulti(layout, plat_len, 6);
		}
	}
}

/**
 * Find a nearby station that joins this station.
 * @tparam T the class to find a station for
 * @tparam error_message the error message when building a station on top of others
 * @tparam F the filter functor type
 * @param existing_station an existing station we build over
 * @param station_to_join the station to join to
 * @param adjacent whether adjacent stations are allowed
 * @param ta the area of the newly build station
 * @param st 'return' pointer for the found station
 * @return command cost with the error or 'okay'
 */
template <class T, StringID error_message, class F>
CommandCost FindJoiningBaseStation(StationID existing_station, StationID station_to_join, bool adjacent, TileArea ta, T **st, F filter)
{
	assert(*st == nullptr);
	bool check_surrounding = true;

	if (existing_station != StationID::Invalid()) {
		if (adjacent && existing_station != station_to_join) {
			/* You can't build an adjacent station over the top of one that
			 * already exists. */
			return CommandCost(error_message);
		} else {
			/* Extend the current station, and don't check whether it will
			 * be near any other stations. */
			T *candidate = T::GetIfValid(existing_station);
			if (candidate != nullptr && filter(candidate)) *st = candidate;
			check_surrounding = (*st == nullptr);
		}
	} else {
		/* There's no station here. Don't check the tiles surrounding this
		 * one if the company wanted to build an adjacent station. */
		if (adjacent) check_surrounding = false;
	}

	if (check_surrounding) {
		/* Make sure there is no more than one other station around us that is owned by us. */
		CommandCost ret = GetStationAround(ta, existing_station, _current_company, st, filter);
		if (ret.Failed()) return ret;
	}

	/* Distant join */
	if (*st == nullptr && station_to_join != StationID::Invalid()) *st = T::GetIfValid(station_to_join);

	return CommandCost();
}

/**
 * Find a nearby station that joins this station.
 * @param existing_station an existing station we build over
 * @param station_to_join the station to join to
 * @param adjacent whether adjacent stations are allowed
 * @param ta the area of the newly build station
 * @param st 'return' pointer for the found station
 * @return command cost with the error or 'okay'
 */
static CommandCost FindJoiningStation(StationID existing_station, StationID station_to_join, bool adjacent, TileArea ta, Station **st)
{
	return FindJoiningBaseStation<Station, STR_ERROR_MUST_REMOVE_RAILWAY_STATION_FIRST>(existing_station, station_to_join, adjacent, ta, st, [](const Station *) -> bool { return true; });
}

/**
 * Find a nearby waypoint that joins this waypoint.
 * @param existing_waypoint an existing waypoint we build over
 * @param waypoint_to_join the waypoint to join to
 * @param adjacent whether adjacent waypoints are allowed
 * @param ta the area of the newly build waypoint
 * @param wp 'return' pointer for the found waypoint
 * @param is_road whether to find a road waypoint
 * @return command cost with the error or 'okay'
 */
CommandCost FindJoiningWaypoint(StationID existing_waypoint, StationID waypoint_to_join, bool adjacent, TileArea ta, Waypoint **wp, bool is_road)
{
	if (is_road) {
		return FindJoiningBaseStation<Waypoint, STR_ERROR_MUST_REMOVE_ROADWAYPOINT_FIRST>(existing_waypoint, waypoint_to_join, adjacent, ta, wp, [](const Waypoint *wp) -> bool { return HasBit(wp->waypoint_flags, WPF_ROAD); });
	} else {
		return FindJoiningBaseStation<Waypoint, STR_ERROR_MUST_REMOVE_RAILWAYPOINT_FIRST>(existing_waypoint, waypoint_to_join, adjacent, ta, wp, [](const Waypoint *wp) -> bool { return !HasBit(wp->waypoint_flags, WPF_ROAD); });
	}
}

/**
 * Clear platform reservation during station building/removing.
 * @param v vehicle which holds reservation
 */
static void FreeTrainReservation(Train *v)
{
	FreeTrainTrackReservation(v);
	if (IsRailStationTile(v->tile)) SetRailStationPlatformReservation(v->tile, TrackdirToExitdir(v->GetVehicleTrackdir()), false);
	v = v->Last();
	if (IsRailStationTile(v->tile)) SetRailStationPlatformReservation(v->tile, TrackdirToExitdir(ReverseTrackdir(v->GetVehicleTrackdir())), false);
}

/**
 * Restore platform reservation during station building/removing.
 * @param v vehicle which held reservation
 */
static void RestoreTrainReservation(Train *v)
{
	if (IsRailStationTile(v->tile)) SetRailStationPlatformReservation(v->tile, TrackdirToExitdir(v->GetVehicleTrackdir()), true);
	TryPathReserve(v, true, true);
	v = v->Last();
	if (IsRailStationTile(v->tile)) SetRailStationPlatformReservation(v->tile, TrackdirToExitdir(ReverseTrackdir(v->GetVehicleTrackdir())), true);
}

/**
 * Calculates cost of new rail stations within the area.
 * @param tile_area Area to check.
 * @param flags Operation to perform.
 * @param axis Rail station axis.
 * @param station StationID to be queried and returned if available.
 * @param rt The rail type to check for (overbuilding rail stations over rail).
 * @param affected_vehicles List of trains with PBS reservations on the tiles
 * @param spec_class Station class.
 * @param spec_index Index into the station class.
 * @param plat_len Platform length.
 * @param numtracks Number of platforms.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CalculateRailStationCost(TileArea tile_area, DoCommandFlags flags, Axis axis, StationID *station, RailType rt, std::vector<Train *> &affected_vehicles, StationClassID spec_class, uint16_t spec_index, uint8_t plat_len, uint8_t numtracks)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	bool length_price_ready = true;
	uint8_t tracknum = 0;
	int allowed_z = -1;
	for (TileIndex cur_tile : tile_area) {
		/* Clear the land below the station. */
		CommandCost ret = CheckFlatLandRailStation(cur_tile, tile_area.tile, allowed_z, flags, axis, station, rt, affected_vehicles, spec_class, spec_index, plat_len, numtracks);
		if (ret.Failed()) return ret;

		/* Only add _price[PR_BUILD_STATION_RAIL_LENGTH] once for each valid plat_len. */
		if (tracknum == numtracks) {
			length_price_ready = true;
			tracknum = 0;
		} else {
			tracknum++;
		}

		/* AddCost for new or rotated rail stations. */
		if (!IsRailStationTile(cur_tile) || (IsRailStationTile(cur_tile) && GetRailStationAxis(cur_tile) != axis)) {
			cost.AddCost(ret.GetCost());
			cost.AddCost(_price[PR_BUILD_STATION_RAIL]);
			cost.AddCost(RailBuildCost(rt));

			if (length_price_ready) {
				cost.AddCost(_price[PR_BUILD_STATION_RAIL_LENGTH]);
				length_price_ready = false;
			}
		}
	}

	return cost;
}

/**
 * Get station tile flags for the given StationGfx.
 * @param gfx StationGfx of station tile.
 * @param statspec Station spec of station tile.
 * @return Tile flags to apply.
 */
static StationSpec::TileFlags GetStationTileFlags(StationGfx gfx, const StationSpec *statspec)
{
	/* Default stations do not draw pylons under roofs (gfx >= 4) */
	if (statspec == nullptr || gfx >= statspec->tileflags.size()) return gfx < 4 ? StationSpec::TileFlag::Pylons : StationSpec::TileFlags{};
	return statspec->tileflags[gfx];
}

/**
 * Set rail station tile flags for the given tile.
 * @param tile Tile to set flags on.
 * @param statspec Statspec of the tile.
 */
void SetRailStationTileFlags(TileIndex tile, const StationSpec *statspec)
{
	const auto flags = GetStationTileFlags(GetStationGfx(tile), statspec);
	SetStationTileBlocked(tile, flags.Test(StationSpec::TileFlag::Blocked));
	SetStationTileHavePylons(tile, flags.Test(StationSpec::TileFlag::Pylons));
	SetStationTileHaveWires(tile, !flags.Test(StationSpec::TileFlag::NoWires));
}

/**
 * Build rail station
 * @param flags operation to perform
 * @param tile_org northern most position of station dragging/placement
 * @param rt railtype
 * @param axis orientation (Axis)
 * @param numtracks number of tracks
 * @param plat_len platform length
 * @param spec_class custom station class
 * @param spec_index custom station id
 * @param station_to_join station ID to join (NEW_STATION if build new one)
 * @param adjacent allow stations directly adjacent to other stations.
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildRailStation(DoCommandFlags flags, TileIndex tile_org, RailType rt, Axis axis, uint8_t numtracks, uint8_t plat_len, StationClassID spec_class, uint16_t spec_index, StationID station_to_join, bool adjacent)
{
	/* Does the authority allow this? */
	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile_org, flags);
	if (ret.Failed()) return ret;

	if (!ValParamRailType(rt) || !IsValidAxis(axis)) return CMD_ERROR;

	/* Check if the given station class is valid */
	if (static_cast<uint>(spec_class) >= StationClass::GetClassCount()) return CMD_ERROR;
	const StationClass *cls = StationClass::Get(spec_class);
	if (IsWaypointClass(*cls)) return CMD_ERROR;
	if (spec_index >= cls->GetSpecCount()) return CMD_ERROR;
	if (plat_len == 0 || numtracks == 0) return CMD_ERROR;

	int w_org, h_org;
	if (axis == AXIS_X) {
		w_org = plat_len;
		h_org = numtracks;
	} else {
		h_org = plat_len;
		w_org = numtracks;
	}

	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (h_org > _settings_game.station.station_spread || w_org > _settings_game.station.station_spread) return CMD_ERROR;

	/* these values are those that will be stored in train_tile and station_platforms */
	TileArea new_location(tile_org, w_org, h_org);

	/* Make sure the area below consists of clear tiles. (OR tiles belonging to a certain rail station) */
	StationID est = StationID::Invalid();
	std::vector<Train *> affected_vehicles;
	/* Add construction and clearing expenses. */
	CommandCost cost = CalculateRailStationCost(new_location, flags, axis, &est, rt, affected_vehicles, spec_class, spec_index, plat_len, numtracks);
	if (cost.Failed()) return cost;

	Station *st = nullptr;
	ret = FindJoiningStation(est, station_to_join, adjacent, new_location, &st);
	if (ret.Failed()) return ret;

	ret = BuildStationPart(&st, flags, reuse, new_location, STATIONNAMING_RAIL);
	if (ret.Failed()) return ret;

	if (st != nullptr && st->train_station.tile != INVALID_TILE) {
		ret = CanExpandRailStation(st, new_location);
		if (ret.Failed()) return ret;
	}

	/* Check if we can allocate a custom stationspec to this station */
	const StationSpec *statspec = StationClass::Get(spec_class)->GetSpec(spec_index);
	int specindex = AllocateSpecToStation(statspec, st, flags.Test(DoCommandFlag::Execute));
	if (specindex == -1) return CommandCost(STR_ERROR_TOO_MANY_STATION_SPECS);

	if (statspec != nullptr) {
		/* Perform NewStation checks */

		/* Check if the station size is permitted */
		if (HasBit(statspec->disallowed_platforms, std::min(numtracks - 1, 7))) return CommandCost(STR_ERROR_STATION_DISALLOWED_NUMBER_TRACKS);
		if (HasBit(statspec->disallowed_lengths, std::min(plat_len - 1, 7))) return CommandCost(STR_ERROR_STATION_DISALLOWED_LENGTH);

		/* Check if the station is buildable */
		if (statspec->callback_mask.Test(StationCallbackMask::Avail)) {
			uint16_t cb_res = GetStationCallback(CBID_STATION_AVAILABILITY, 0, 0, statspec, nullptr, INVALID_TILE);
			if (cb_res != CALLBACK_FAILED && !Convert8bitBooleanCallback(statspec->grf_prop.grffile, CBID_STATION_AVAILABILITY, cb_res)) return CMD_ERROR;
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		st->train_station = new_location;
		st->AddFacility(StationFacility::Train, new_location.tile);

		st->rect.BeforeAddRect(tile_org, w_org, h_org, StationRect::ADD_TRY);

		if (statspec != nullptr) {
			/* Include this station spec's animation trigger bitmask
			 * in the station's cached copy. */
			st->cached_anim_triggers.Set(statspec->animation.triggers);
		}

		TileIndexDiff tile_delta = TileOffsByAxis(axis); // offset to go to the next platform tile
		TileIndexDiff track_delta = TileOffsByAxis(OtherAxis(axis)); // offset to go to the next track
		Track track = AxisToTrack(axis);

		std::vector<uint8_t> layouts(numtracks * plat_len);
		GetStationLayout(layouts.data(), numtracks, plat_len, statspec);

		uint8_t numtracks_orig = numtracks;

		Company *c = Company::Get(st->owner);
		size_t layout_idx = 0;
		TileIndex tile_track = tile_org;
		do {
			TileIndex tile = tile_track;
			int w = plat_len;
			do {
				uint8_t layout = layouts[layout_idx++];
				if (IsRailStationTile(tile) && HasStationReservation(tile)) {
					/* Check for trains having a reservation for this tile. */
					Train *v = GetTrainForReservation(tile, AxisToTrack(GetRailStationAxis(tile)));
					if (v != nullptr) {
						affected_vehicles.push_back(v);
						FreeTrainReservation(v);
					}
				}

				/* Railtype can change when overbuilding. */
				if (IsRailStationTile(tile)) {
					if (!IsStationTileBlocked(tile)) c->infrastructure.rail[GetRailType(tile)]--;
					c->infrastructure.station--;
				}

				/* Remove animation if overbuilding */
				DeleteAnimatedTile(tile);
				uint8_t old_specindex = HasStationTileRail(tile) ? GetCustomStationSpecIndex(tile) : 0;
				MakeRailStation(tile, st->owner, st->index, axis, layout & ~1, rt);
				/* Free the spec if we overbuild something */
				DeallocateSpecFromStation(st, old_specindex);

				SetCustomStationSpecIndex(tile, specindex);
				SetStationTileRandomBits(tile, GB(Random(), 0, 4));
				SetAnimationFrame(tile, 0);

				if (statspec != nullptr) {
					/* Use a fixed axis for GetPlatformInfo as our platforms / numtracks are always the right way around */
					uint32_t platinfo = GetPlatformInfo(AXIS_X, GetStationGfx(tile), plat_len, numtracks_orig, plat_len - w, numtracks_orig - numtracks, false);

					/* As the station is not yet completely finished, the station does not yet exist. */
					uint16_t callback = GetStationCallback(CBID_STATION_BUILD_TILE_LAYOUT, platinfo, 0, statspec, nullptr, tile);
					if (callback != CALLBACK_FAILED) {
						if (callback <= UINT8_MAX) {
							SetStationGfx(tile, (callback & ~1) + axis);
						} else {
							ErrorUnknownCallbackResult(statspec->grf_prop.grfid, CBID_STATION_BUILD_TILE_LAYOUT, callback);
						}
					}

					/* Trigger station animation -- after building? */
					TriggerStationAnimation(st, tile, StationAnimationTrigger::Built);
				}

				SetRailStationTileFlags(tile, statspec);

				if (!IsStationTileBlocked(tile)) c->infrastructure.rail[rt]++;
				c->infrastructure.station++;

				tile += tile_delta;
			} while (--w);
			AddTrackToSignalBuffer(tile_track, track, _current_company);
			YapfNotifyTrackLayoutChange(tile_track, track);
			tile_track += track_delta;
		} while (--numtracks);

		for (uint i = 0; i < affected_vehicles.size(); ++i) {
			/* Restore reservations of trains. */
			RestoreTrainReservation(affected_vehicles[i]);
		}

		/* Check whether we need to expand the reservation of trains already on the station. */
		TileArea update_reservation_area;
		if (axis == AXIS_X) {
			update_reservation_area = TileArea(tile_org, 1, numtracks_orig);
		} else {
			update_reservation_area = TileArea(tile_org, numtracks_orig, 1);
		}

		for (TileIndex tile : update_reservation_area) {
			/* Don't even try to make eye candy parts reserved. */
			if (IsStationTileBlocked(tile)) continue;

			DiagDirection dir = AxisToDiagDir(axis);
			TileIndexDiff tile_offset = TileOffsByDiagDir(dir);
			TileIndex platform_begin = tile;
			TileIndex platform_end = tile;

			/* We can only account for tiles that are reachable from this tile, so ignore primarily blocked tiles while finding the platform begin and end. */
			for (TileIndex next_tile = platform_begin - tile_offset; IsCompatibleTrainStationTile(next_tile, platform_begin); next_tile -= tile_offset) {
				platform_begin = next_tile;
			}
			for (TileIndex next_tile = platform_end + tile_offset; IsCompatibleTrainStationTile(next_tile, platform_end); next_tile += tile_offset) {
				platform_end = next_tile;
			}

			/* If there is at least on reservation on the platform, we reserve the whole platform. */
			bool reservation = false;
			for (TileIndex t = platform_begin; !reservation && t <= platform_end; t += tile_offset) {
				reservation = HasStationReservation(t);
			}

			if (reservation) {
				SetRailStationPlatformReservation(platform_begin, dir, true);
			}
		}

		st->MarkTilesDirty(false);
		st->AfterStationTileSetChange(true, StationType::Rail);
	}

	return cost;
}

static TileArea MakeStationAreaSmaller(BaseStation *st, TileArea ta, bool (*func)(BaseStation *, TileIndex))
{
restart:

	/* too small? */
	if (ta.w != 0 && ta.h != 0) {
		/* check the left side, x = constant, y changes */
		for (uint i = 0; !func(st, ta.tile + TileDiffXY(0, i));) {
			/* the left side is unused? */
			if (++i == ta.h) {
				ta.tile += TileDiffXY(1, 0);
				ta.w--;
				goto restart;
			}
		}

		/* check the right side, x = constant, y changes */
		for (uint i = 0; !func(st, ta.tile + TileDiffXY(ta.w - 1, i));) {
			/* the right side is unused? */
			if (++i == ta.h) {
				ta.w--;
				goto restart;
			}
		}

		/* check the upper side, y = constant, x changes */
		for (uint i = 0; !func(st, ta.tile + TileDiffXY(i, 0));) {
			/* the left side is unused? */
			if (++i == ta.w) {
				ta.tile += TileDiffXY(0, 1);
				ta.h--;
				goto restart;
			}
		}

		/* check the lower side, y = constant, x changes */
		for (uint i = 0; !func(st, ta.tile + TileDiffXY(i, ta.h - 1));) {
			/* the left side is unused? */
			if (++i == ta.w) {
				ta.h--;
				goto restart;
			}
		}
	} else {
		ta.Clear();
	}

	return ta;
}

static bool TileBelongsToRailStation(BaseStation *st, TileIndex tile)
{
	return st->TileBelongsToRailStation(tile);
}

static void MakeRailStationAreaSmaller(BaseStation *st)
{
	st->train_station = MakeStationAreaSmaller(st, st->train_station, TileBelongsToRailStation);
}

static bool TileBelongsToShipStation(BaseStation *st, TileIndex tile)
{
	return IsDockTile(tile) && GetStationIndex(tile) == st->index;
}

static void MakeShipStationAreaSmaller(Station *st)
{
	st->ship_station = MakeStationAreaSmaller(st, st->ship_station, TileBelongsToShipStation);
	UpdateStationDockingTiles(st);
}

static bool TileBelongsToRoadWaypointStation(BaseStation *st, TileIndex tile)
{
	return IsRoadWaypointTile(tile) && GetStationIndex(tile) == st->index;
}

void MakeRoadWaypointStationAreaSmaller(BaseStation *st, TileArea &road_waypoint_area)
{
	road_waypoint_area = MakeStationAreaSmaller(st, road_waypoint_area, TileBelongsToRoadWaypointStation);
}

/**
 * Remove a number of tiles from any rail station within the area.
 * @param ta the area to clear station tile from.
 * @param affected_stations the stations affected.
 * @param flags the command flags.
 * @param removal_cost the cost for removing the tile, including the rail.
 * @param keep_rail whether to keep the rail of the station.
 * @tparam T the type of station to remove.
 * @return the number of cleared tiles or an error.
 */
template <class T>
CommandCost RemoveFromRailBaseStation(TileArea ta, std::vector<T *> &affected_stations, DoCommandFlags flags, Money removal_cost, bool keep_rail)
{
	/* Count of the number of tiles removed */
	int quantity = 0;
	CommandCost total_cost(EXPENSES_CONSTRUCTION);
	/* Accumulator for the errors seen during clearing. If no errors happen,
	 * and the quantity is 0 there is no station. Otherwise it will be one
	 * of the other error that got accumulated. */
	CommandCost error;

	/* Do the action for every tile into the area */
	for (TileIndex tile : ta) {
		/* Make sure the specified tile is a rail station */
		if (!HasStationTileRail(tile)) continue;

		/* If there is a vehicle on ground, do not allow to remove (flood) the tile */
		CommandCost ret = EnsureNoVehicleOnGround(tile);
		error.AddCost(std::move(ret));
		if (error.Failed()) continue;

		/* Check ownership of station */
		T *st = T::GetByTile(tile);
		if (st == nullptr) continue;

		if (_current_company != OWNER_WATER) {
			ret = CheckOwnership(st->owner);
			error.AddCost(std::move(ret));
			if (error.Failed()) continue;
		}

		/* If we reached here, the tile is valid so increase the quantity of tiles we will remove */
		quantity++;

		if (keep_rail || IsStationTileBlocked(tile)) {
			/* Don't refund the 'steel' of the track when we keep the
			 *  rail, or when the tile didn't have any rail at all. */
			total_cost.AddCost(-_price[PR_CLEAR_RAIL]);
		}

		if (flags.Test(DoCommandFlag::Execute)) {
			/* read variables before the station tile is removed */
			uint specindex = GetCustomStationSpecIndex(tile);
			Track track = GetRailStationTrack(tile);
			Owner owner = GetTileOwner(tile);
			RailType rt = GetRailType(tile);
			Train *v = nullptr;

			if (HasStationReservation(tile)) {
				v = GetTrainForReservation(tile, track);
				if (v != nullptr) FreeTrainReservation(v);
			}

			bool build_rail = keep_rail && !IsStationTileBlocked(tile);
			if (!build_rail && !IsStationTileBlocked(tile)) Company::Get(owner)->infrastructure.rail[rt]--;

			DoClearSquare(tile);
			DeleteNewGRFInspectWindow(GSF_STATIONS, tile.base());
			if (build_rail) MakeRailNormal(tile, owner, TrackToTrackBits(track), rt);
			Company::Get(owner)->infrastructure.station--;
			DirtyCompanyInfrastructureWindows(owner);

			st->rect.AfterRemoveTile(st, tile);
			AddTrackToSignalBuffer(tile, track, owner);
			YapfNotifyTrackLayoutChange(tile, track);

			DeallocateSpecFromStation(st, specindex);

			include(affected_stations, st);

			if (v != nullptr) RestoreTrainReservation(v);
		}
	}

	if (quantity == 0) return error.Failed() ? error : CommandCost(STR_ERROR_THERE_IS_NO_STATION);

	for (T *st : affected_stations) {

		/* now we need to make the "spanned" area of the railway station smaller
		 * if we deleted something at the edges.
		 * we also need to adjust train_tile. */
		MakeRailStationAreaSmaller(st);
		UpdateStationSignCoord(st);

		/* if we deleted the whole station, delete the train facility. */
		if (st->train_station.tile == INVALID_TILE) {
			st->facilities.Reset(StationFacility::Train);
			SetWindowClassesDirty(WC_VEHICLE_ORDERS);
			SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_TRAINS);
			MarkCatchmentTilesDirty();
			st->UpdateVirtCoord();
			DeleteStationIfEmpty(st);
		}
	}

	total_cost.AddCost(quantity * removal_cost);
	return total_cost;
}

/**
 * Remove a single tile from a rail station.
 * This allows for custom-built station with holes and weird layouts
 * @param flags operation to perform
 * @param start tile of station piece to remove
 * @param end other edge of the rect to remove
 * @param keep_rail if set keep the rail
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveFromRailStation(DoCommandFlags flags, TileIndex start, TileIndex end, bool keep_rail)
{
	if (end == 0) end = start;
	if (start >= Map::Size() || end >= Map::Size()) return CMD_ERROR;

	TileArea ta(start, end);
	std::vector<Station *> affected_stations;

	CommandCost ret = RemoveFromRailBaseStation(ta, affected_stations, flags, _price[PR_CLEAR_STATION_RAIL], keep_rail);
	if (ret.Failed()) return ret;

	/* Do all station specific functions here. */
	for (Station *st : affected_stations) {

		if (st->train_station.tile == INVALID_TILE) SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_TRAINS);
		st->MarkTilesDirty(false);
		MarkCatchmentTilesDirty();
		st->RecomputeCatchment();
	}

	/* Now apply the rail cost to the number that we deleted */
	return ret;
}

/**
 * Remove a single tile from a waypoint.
 * This allows for custom-built waypoint with holes and weird layouts
 * @param flags operation to perform
 * @param start tile of waypoint piece to remove
 * @param end other edge of the rect to remove
 * @param keep_rail if set keep the rail
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveFromRailWaypoint(DoCommandFlags flags, TileIndex start, TileIndex end, bool keep_rail)
{
	if (end == 0) end = start;
	if (start >= Map::Size() || end >= Map::Size()) return CMD_ERROR;

	TileArea ta(start, end);
	std::vector<Waypoint *> affected_stations;

	return RemoveFromRailBaseStation(ta, affected_stations, flags, _price[PR_CLEAR_WAYPOINT_RAIL], keep_rail);
}


/**
 * Remove a rail station/waypoint
 * @param st The station/waypoint to remove the rail part from
 * @param flags operation to perform
 * @param removal_cost the cost for removing a tile
 * @tparam T the type of station to remove
 * @return cost or failure of operation
 */
template <class T>
CommandCost RemoveRailStation(T *st, DoCommandFlags flags, Money removal_cost)
{
	/* Current company owns the station? */
	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	/* determine width and height of platforms */
	TileArea ta = st->train_station;

	assert(ta.w != 0 && ta.h != 0);

	CommandCost cost(EXPENSES_CONSTRUCTION);
	/* clear all areas of the station */
	for (TileIndex tile : ta) {
		/* only remove tiles that are actually train station tiles */
		if (st->TileBelongsToRailStation(tile)) {
			std::vector<T*> affected_stations; // dummy
			CommandCost ret = RemoveFromRailBaseStation(TileArea(tile, 1, 1), affected_stations, flags, removal_cost, false);
			if (ret.Failed()) return ret;
			cost.AddCost(ret.GetCost());
		}
	}

	return cost;
}

/**
 * Remove a rail station
 * @param tile Tile of the station.
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveRailStation(TileIndex tile, DoCommandFlags flags)
{
	/* if there is flooding, remove platforms tile by tile */
	if (_current_company == OWNER_WATER) {
		return Command<CMD_REMOVE_FROM_RAIL_STATION>::Do(DoCommandFlag::Execute, tile, TileIndex{}, false);
	}

	Station *st = Station::GetByTile(tile);
	CommandCost cost = RemoveRailStation(st, flags, _price[PR_CLEAR_STATION_RAIL]);

	if (flags.Test(DoCommandFlag::Execute)) st->RecomputeCatchment();

	return cost;
}

/**
 * Remove a rail waypoint
 * @param tile Tile of the waypoint.
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveRailWaypoint(TileIndex tile, DoCommandFlags flags)
{
	/* if there is flooding, remove waypoints tile by tile */
	if (_current_company == OWNER_WATER) {
		return Command<CMD_REMOVE_FROM_RAIL_WAYPOINT>::Do(DoCommandFlag::Execute, tile, TileIndex{}, false);
	}

	return RemoveRailStation(Waypoint::GetByTile(tile), flags, _price[PR_CLEAR_WAYPOINT_RAIL]);
}


/**
 * @param truck_station Determines whether a stop is #RoadStopType::Bus or #RoadStopType::Truck
 * @param st The Station to do the whole procedure for
 * @return a pointer to where to link a new RoadStop*
 */
static RoadStop **FindRoadStopSpot(bool truck_station, Station *st)
{
	RoadStop **primary_stop = (truck_station) ? &st->truck_stops : &st->bus_stops;

	if (*primary_stop == nullptr) {
		/* we have no roadstop of the type yet, so write a "primary stop" */
		return primary_stop;
	} else {
		/* there are stops already, so append to the end of the list */
		RoadStop *stop = *primary_stop;
		while (stop->next != nullptr) stop = stop->next;
		return &stop->next;
	}
}

static CommandCost RemoveRoadStop(TileIndex tile, DoCommandFlags flags, int replacement_spec_index = -1);
CommandCost RemoveRoadWaypointStop(TileIndex tile, DoCommandFlags flags, int replacement_spec_index = -1);

/**
 * Find a nearby station that joins this road stop.
 * @param existing_stop an existing road stop we build over
 * @param station_to_join the station to join to
 * @param adjacent whether adjacent stations are allowed
 * @param ta the area of the newly build station
 * @param st 'return' pointer for the found station
 * @return command cost with the error or 'okay'
 */
static CommandCost FindJoiningRoadStop(StationID existing_stop, StationID station_to_join, bool adjacent, TileArea ta, Station **st)
{
	return FindJoiningBaseStation<Station, STR_ERROR_MUST_REMOVE_ROAD_STOP_FIRST>(existing_stop, station_to_join, adjacent, ta, st, [](const Station *) -> bool { return true; });
}

/**
 * Calculates cost of new road stops within the area.
 * @param tile_area Area to check.
 * @param flags Operation to perform.
 * @param is_drive_through True if trying to build a drive-through station.
 * @param station_type Station type (bus, truck or road waypoint).
 * @param axis Axis of a drive-through road stop.
 * @param ddir Entrance direction (#DiagDirection) for normal stops. Converted to the axis for drive-through stops.
 * @param station StationID to be queried and returned if available.
 * @param rt Road type to build, may be INVALID_ROADTYPE if an existing road is required.
 * @param unit_cost The cost to build one road stop of the current type.
 * @return The cost in case of success, or an error code if it failed.
 */
CommandCost CalculateRoadStopCost(TileArea tile_area, DoCommandFlags flags, bool is_drive_through, StationType station_type, Axis axis, DiagDirection ddir, StationID *est, RoadType rt, Money unit_cost)
{
	DiagDirections invalid_dirs{};
	if (is_drive_through) {
		invalid_dirs.Set(AxisToDiagDir(axis));
		invalid_dirs.Set(ReverseDiagDir(AxisToDiagDir(axis)));
	} else {
		invalid_dirs.Set(ddir);
	}

	/* Check every tile in the area. */
	int allowed_z = -1;
	CommandCost cost(EXPENSES_CONSTRUCTION);
	for (TileIndex cur_tile : tile_area) {
		CommandCost ret = CheckFlatLandRoadStop(cur_tile, allowed_z, flags, invalid_dirs, is_drive_through, station_type, axis, est, rt);
		if (ret.Failed()) return ret;

		bool is_preexisting_roadstop = IsTileType(cur_tile, MP_STATION) && IsAnyRoadStop(cur_tile);

		/* Only add costs if a stop doesn't already exist in the location */
		if (!is_preexisting_roadstop) {
			cost.AddCost(ret.GetCost());
			cost.AddCost(unit_cost);
		}
	}

	return cost;
}

/**
 * Build a bus or truck stop.
 * @param flags Operation to perform.
 * @param tile Northernmost tile of the stop.
 * @param width Width of the road stop.
 * @param length Length of the road stop.
 * @param stop_type Type of road stop (bus/truck).
 * @param is_drive_through False for normal stops, true for drive-through.
 * @param ddir Entrance direction (#DiagDirection) for normal stops. Converted to the axis for drive-through stops.
 * @param rt The roadtype.
 * @param spec_class Road stop spec class.
 * @param spec_index Road stop spec index.
 * @param station_to_join Station ID to join (NEW_STATION if build new one).
 * @param adjacent Allow stations directly adjacent to other stations.
 * @return The cost of this operation or an error.
 */
CommandCost CmdBuildRoadStop(DoCommandFlags flags, TileIndex tile, uint8_t width, uint8_t length, RoadStopType stop_type, bool is_drive_through,
		DiagDirection ddir, RoadType rt, RoadStopClassID spec_class, uint16_t spec_index, StationID station_to_join, bool adjacent)
{
	if (!ValParamRoadType(rt) || !IsValidDiagDirection(ddir) || stop_type >= RoadStopType::End) return CMD_ERROR;
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());

	/* Check if the given station class is valid */
	if (static_cast<uint>(spec_class) >= RoadStopClass::GetClassCount()) return CMD_ERROR;
	const RoadStopClass *cls = RoadStopClass::Get(spec_class);
	if (IsWaypointClass(*cls)) return CMD_ERROR;
	if (spec_index >= cls->GetSpecCount()) return CMD_ERROR;

	const RoadStopSpec *roadstopspec = cls->GetSpec(spec_index);
	if (roadstopspec != nullptr) {
		if (stop_type == RoadStopType::Truck && roadstopspec->stop_type != ROADSTOPTYPE_FREIGHT && roadstopspec->stop_type != ROADSTOPTYPE_ALL) return CMD_ERROR;
		if (stop_type == RoadStopType::Bus && roadstopspec->stop_type != ROADSTOPTYPE_PASSENGER && roadstopspec->stop_type != ROADSTOPTYPE_ALL) return CMD_ERROR;
		if (!is_drive_through && roadstopspec->flags.Test(RoadStopSpecFlag::DriveThroughOnly)) return CMD_ERROR;
	}

	/* Check if the requested road stop is too big */
	if (width > _settings_game.station.station_spread || length > _settings_game.station.station_spread) return CommandCost(STR_ERROR_STATION_TOO_SPREAD_OUT);
	/* Check for incorrect width / length. */
	if (width == 0 || length == 0) return CMD_ERROR;
	/* Check if the first tile and the last tile are valid */
	if (!IsValidTile(tile) || TileAddWrap(tile, width - 1, length - 1) == INVALID_TILE) return CMD_ERROR;

	TileArea roadstop_area(tile, width, length);

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	/* Trams only have drive through stops */
	if (!is_drive_through && RoadTypeIsTram(rt)) return CMD_ERROR;

	Axis axis = DiagDirToAxis(ddir);

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	bool is_truck_stop = stop_type != RoadStopType::Bus;

	/* Total road stop cost. */
	Money unit_cost;
	if (roadstopspec != nullptr) {
		unit_cost = roadstopspec->GetBuildCost(is_truck_stop ? PR_BUILD_STATION_TRUCK : PR_BUILD_STATION_BUS);
	} else {
		unit_cost = _price[is_truck_stop ? PR_BUILD_STATION_TRUCK : PR_BUILD_STATION_BUS];
	}
	StationID est = StationID::Invalid();
	CommandCost cost = CalculateRoadStopCost(roadstop_area, flags, is_drive_through, is_truck_stop ? StationType::Truck : StationType::Bus, axis, ddir, &est, rt, unit_cost);
	if (cost.Failed()) return cost;

	Station *st = nullptr;
	ret = FindJoiningRoadStop(est, station_to_join, adjacent, roadstop_area, &st);
	if (ret.Failed()) return ret;

	/* Check if this number of road stops can be allocated. */
	if (!RoadStop::CanAllocateItem(static_cast<size_t>(roadstop_area.w) * roadstop_area.h)) return CommandCost(is_truck_stop ? STR_ERROR_TOO_MANY_TRUCK_STOPS : STR_ERROR_TOO_MANY_BUS_STOPS);

	ret = BuildStationPart(&st, flags, reuse, roadstop_area, STATIONNAMING_ROAD);
	if (ret.Failed()) return ret;

	/* Check if we can allocate a custom stationspec to this station */
	int specindex = AllocateSpecToRoadStop(roadstopspec, st, flags.Test(DoCommandFlag::Execute));
	if (specindex == -1) return CommandCost(STR_ERROR_TOO_MANY_STATION_SPECS);

	if (roadstopspec != nullptr) {
		/* Perform NewGRF checks */

		/* Check if the road stop is buildable */
		if (roadstopspec->callback_mask.Test(RoadStopCallbackMask::Avail)) {
			uint16_t cb_res = GetRoadStopCallback(CBID_STATION_AVAILABILITY, 0, 0, roadstopspec, nullptr, INVALID_TILE, rt, is_truck_stop ? StationType::Truck : StationType::Bus, 0);
			if (cb_res != CALLBACK_FAILED && !Convert8bitBooleanCallback(roadstopspec->grf_prop.grffile, CBID_STATION_AVAILABILITY, cb_res)) return CMD_ERROR;
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Check every tile in the area. */
		for (TileIndex cur_tile : roadstop_area) {
			/* Get existing road types and owners before any tile clearing */
			RoadType road_rt = MayHaveRoad(cur_tile) ? GetRoadType(cur_tile, RTT_ROAD) : INVALID_ROADTYPE;
			RoadType tram_rt = MayHaveRoad(cur_tile) ? GetRoadType(cur_tile, RTT_TRAM) : INVALID_ROADTYPE;
			Owner road_owner = road_rt != INVALID_ROADTYPE ? GetRoadOwner(cur_tile, RTT_ROAD) : _current_company;
			Owner tram_owner = tram_rt != INVALID_ROADTYPE ? GetRoadOwner(cur_tile, RTT_TRAM) : _current_company;

			if (IsTileType(cur_tile, MP_STATION) && IsStationRoadStop(cur_tile)) {
				RemoveRoadStop(cur_tile, flags, specindex);
			}

			if (roadstopspec != nullptr) {
				/* Include this road stop spec's animation trigger bitmask
				 * in the station's cached copy. */
				st->cached_roadstop_anim_triggers.Set(roadstopspec->animation.triggers);
			}

			RoadStop *road_stop = new RoadStop(cur_tile);
			/* Insert into linked list of RoadStops. */
			RoadStop **currstop = FindRoadStopSpot(is_truck_stop, st);
			*currstop = road_stop;

			if (is_truck_stop) {
				st->truck_station.Add(cur_tile);
			} else {
				st->bus_station.Add(cur_tile);
			}

			/* Initialize an empty station. */
			st->AddFacility(is_truck_stop ? StationFacility::TruckStop : StationFacility::BusStop, cur_tile);

			st->rect.BeforeAddTile(cur_tile, StationRect::ADD_TRY);

			RoadStopType rs_type = is_truck_stop ? RoadStopType::Truck : RoadStopType::Bus;
			if (is_drive_through) {
				/* Update company infrastructure counts. If the current tile is a normal road tile, remove the old
				 * bits first. */
				if (IsNormalRoadTile(cur_tile)) {
					UpdateCompanyRoadInfrastructure(road_rt, road_owner, -(int)CountBits(GetRoadBits(cur_tile, RTT_ROAD)));
					UpdateCompanyRoadInfrastructure(tram_rt, tram_owner, -(int)CountBits(GetRoadBits(cur_tile, RTT_TRAM)));
				}

				if (road_rt == INVALID_ROADTYPE && RoadTypeIsRoad(rt)) road_rt = rt;
				if (tram_rt == INVALID_ROADTYPE && RoadTypeIsTram(rt)) tram_rt = rt;

				MakeDriveThroughRoadStop(cur_tile, st->owner, road_owner, tram_owner, st->index, (rs_type == RoadStopType::Bus ? StationType::Bus : StationType::Truck), road_rt, tram_rt, axis);
				road_stop->MakeDriveThrough();
			} else {
				if (road_rt == INVALID_ROADTYPE && RoadTypeIsRoad(rt)) road_rt = rt;
				if (tram_rt == INVALID_ROADTYPE && RoadTypeIsTram(rt)) tram_rt = rt;
				MakeRoadStop(cur_tile, st->owner, st->index, rs_type, road_rt, tram_rt, ddir);
			}
			UpdateCompanyRoadInfrastructure(road_rt, road_owner, ROAD_STOP_TRACKBIT_FACTOR);
			UpdateCompanyRoadInfrastructure(tram_rt, tram_owner, ROAD_STOP_TRACKBIT_FACTOR);
			Company::Get(st->owner)->infrastructure.station++;

			SetCustomRoadStopSpecIndex(cur_tile, specindex);
			if (roadstopspec != nullptr) {
				st->SetRoadStopRandomBits(cur_tile, GB(Random(), 0, 8));
				TriggerRoadStopAnimation(st, cur_tile, StationAnimationTrigger::Built);
			}

			MarkTileDirtyByTile(cur_tile);
		}

		if (st != nullptr) {
			st->AfterStationTileSetChange(true, is_truck_stop ? StationType::Truck: StationType::Bus);
		}
	}
	return cost;
}

/**
 * Remove a bus station/truck stop
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @param replacement_spec_index replacement spec index to avoid deallocating, if < 0, tile is not being replaced
 * @return cost or failure of operation
 */
static CommandCost RemoveRoadStop(TileIndex tile, DoCommandFlags flags, int replacement_spec_index)
{
	Station *st = Station::GetByTile(tile);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	bool is_truck = IsTruckStop(tile);

	RoadStop **primary_stop;
	RoadStop *cur_stop;
	if (is_truck) { // truck stop
		primary_stop = &st->truck_stops;
		cur_stop = RoadStop::GetByTile(tile, RoadStopType::Truck);
	} else {
		primary_stop = &st->bus_stops;
		cur_stop = RoadStop::GetByTile(tile, RoadStopType::Bus);
	}

	assert(cur_stop != nullptr);

	/* don't do the check for drive-through road stops when company bankrupts */
	if (IsDriveThroughStopTile(tile) && flags.Test(DoCommandFlag::Bankrupt)) {
		/* remove the 'going through road stop' status from all vehicles on that tile */
		if (flags.Test(DoCommandFlag::Execute)) {
			for (Vehicle *v : VehiclesOnTile(tile)) {
				if (v->type != VEH_ROAD) continue;
				/* Okay... we are a road vehicle on a drive through road stop.
				 * But that road stop has just been removed, so we need to make
				 * sure we are in a valid state... however, vehicles can also
				 * turn on road stop tiles, so only clear the 'road stop' state
				 * bits and only when the state was 'in road stop', otherwise
				 * we'll end up clearing the turn around bits. */
				RoadVehicle *rv = RoadVehicle::From(v);
				if (HasBit(rv->state, RVS_IN_DT_ROAD_STOP)) rv->state &= RVSB_ROAD_STOP_TRACKDIR_MASK;
			}
		}
	} else {
		CommandCost ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;
	}

	const RoadStopSpec *spec = GetRoadStopSpec(tile);

	if (flags.Test(DoCommandFlag::Execute)) {
		if (*primary_stop == cur_stop) {
			/* removed the first stop in the list */
			*primary_stop = cur_stop->next;
			/* removed the only stop? */
			if (*primary_stop == nullptr) {
				st->facilities.Reset(is_truck ? StationFacility::TruckStop : StationFacility::BusStop);
				SetWindowClassesDirty(WC_VEHICLE_ORDERS);
			}
		} else {
			/* tell the predecessor in the list to skip this stop */
			RoadStop *pred = *primary_stop;
			while (pred->next != cur_stop) pred = pred->next;
			pred->next = cur_stop->next;
		}

		/* Update company infrastructure counts. */
		for (RoadTramType rtt : _roadtramtypes) {
			RoadType rt = GetRoadType(tile, rtt);
			UpdateCompanyRoadInfrastructure(rt, GetRoadOwner(tile, rtt), -static_cast<int>(ROAD_STOP_TRACKBIT_FACTOR));
		}

		Company::Get(st->owner)->infrastructure.station--;
		DirtyCompanyInfrastructureWindows(st->owner);

		uint specindex = GetCustomRoadStopSpecIndex(tile);

		DeleteNewGRFInspectWindow(GSF_ROADSTOPS, tile.base());

		if (IsDriveThroughStopTile(tile)) {
			/* Clears the tile for us */
			cur_stop->ClearDriveThrough();
			DeleteAnimatedTile(tile);
		} else {
			DoClearSquare(tile);
		}

		delete cur_stop;

		/* Make sure no vehicle is going to the old roadstop. Narrow the search to any road vehicles with an order to
		 * this station, then look for any currently heading to the tile. */
		StationID station_id = st->index;
		FindVehiclesWithOrder(
			[](const Vehicle *v) { return v->type == VEH_ROAD; },
			[station_id](const Order *order) { return order->IsType(OT_GOTO_STATION) && order->GetDestination() == station_id; },
			[station_id, tile](Vehicle *v) {
				if (v->current_order.IsType(OT_GOTO_STATION) && v->dest_tile == tile) {
					v->SetDestTile(v->GetOrderStationLocation(station_id));
				}
			}
		);

		st->rect.AfterRemoveTile(st, tile);

		if (replacement_spec_index < 0) st->AfterStationTileSetChange(false, is_truck ? StationType::Truck: StationType::Bus);

		st->RemoveRoadStopTileData(tile);
		if ((int)specindex != replacement_spec_index) DeallocateSpecFromRoadStop(st, specindex);

		/* Update the tile area of the truck/bus stop */
		if (is_truck) {
			st->truck_station.Clear();
			for (const RoadStop *rs = st->truck_stops; rs != nullptr; rs = rs->next) st->truck_station.Add(rs->xy);
		} else {
			st->bus_station.Clear();
			for (const RoadStop *rs = st->bus_stops; rs != nullptr; rs = rs->next) st->bus_station.Add(rs->xy);
		}
	}

	Price category = is_truck ? PR_CLEAR_STATION_TRUCK : PR_CLEAR_STATION_BUS;
	return CommandCost(EXPENSES_CONSTRUCTION, spec != nullptr ? spec->GetClearCost(category) : _price[category]);
}

/**
 * Remove a road waypoint
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @param replacement_spec_index replacement spec index to avoid deallocating, if < 0, tile is not being replaced
 * @return cost or failure of operation
 */
CommandCost RemoveRoadWaypointStop(TileIndex tile, DoCommandFlags flags, int replacement_spec_index)
{
	Waypoint *wp = Waypoint::GetByTile(tile);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(wp->owner);
		if (ret.Failed()) return ret;
	}

	/* Ignore vehicles when the company goes bankrupt. The road will remain, any vehicles going to the waypoint will be removed. */
	if (!flags.Test(DoCommandFlag::Bankrupt)) {
		CommandCost ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;
	}

	const RoadStopSpec *spec = GetRoadStopSpec(tile);

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Update company infrastructure counts. */
		for (RoadTramType rtt : _roadtramtypes) {
			RoadType rt = GetRoadType(tile, rtt);
			UpdateCompanyRoadInfrastructure(rt, GetRoadOwner(tile, rtt), -static_cast<int>(ROAD_STOP_TRACKBIT_FACTOR));
		}

		Company::Get(wp->owner)->infrastructure.station--;
		DirtyCompanyInfrastructureWindows(wp->owner);

		uint specindex = GetCustomRoadStopSpecIndex(tile);

		DeleteNewGRFInspectWindow(GSF_ROADSTOPS, tile.base());

		DoClearSquare(tile);

		wp->rect.AfterRemoveTile(wp, tile);

		wp->RemoveRoadStopTileData(tile);
		if ((int)specindex != replacement_spec_index) DeallocateSpecFromRoadStop(wp, specindex);

		if (replacement_spec_index < 0) {
			MakeRoadWaypointStationAreaSmaller(wp, wp->road_waypoint_area);

			UpdateStationSignCoord(wp);

			/* if we deleted the whole waypoint, delete the road facility. */
			if (wp->road_waypoint_area.tile == INVALID_TILE) {
				wp->facilities.Reset({StationFacility::BusStop, StationFacility::TruckStop});
				SetWindowWidgetDirty(WC_STATION_VIEW, wp->index, WID_SV_ROADVEHS);
				wp->UpdateVirtCoord();
				DeleteStationIfEmpty(wp);
			}
		}
	}

	return CommandCost(EXPENSES_CONSTRUCTION, spec != nullptr ? spec->GetClearCost(PR_CLEAR_STATION_TRUCK) : _price[PR_CLEAR_STATION_TRUCK]);
}

/**
 * Remove a tile area of road stop or road waypoints
 * @param flags operation to perform
 * @param roadstop_area tile area of road stop or road waypoint tiles to remove
 * @param road_waypoint Whether to remove road waypoints or road stops
 * @param remove_road Remove roads of drive-through stops?
 * @return the cost of this operation or an error
 */
static CommandCost RemoveGenericRoadStop(DoCommandFlags flags, const TileArea &roadstop_area, bool road_waypoint, bool remove_road)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	CommandCost last_error(STR_ERROR_THERE_IS_NO_STATION);
	bool had_success = false;

	for (TileIndex cur_tile : roadstop_area) {
		/* Make sure the specified tile is a road stop of the correct type */
		if (!IsTileType(cur_tile, MP_STATION) || !IsAnyRoadStop(cur_tile) || IsRoadWaypoint(cur_tile) != road_waypoint) continue;

		/* Save information on to-be-restored roads before the stop is removed. */
		RoadBits road_bits = ROAD_NONE;
		RoadType road_type[] = { INVALID_ROADTYPE, INVALID_ROADTYPE };
		Owner road_owner[] = { OWNER_NONE, OWNER_NONE };
		if (IsDriveThroughStopTile(cur_tile)) {
			for (RoadTramType rtt : _roadtramtypes) {
				road_type[rtt] = GetRoadType(cur_tile, rtt);
				if (road_type[rtt] == INVALID_ROADTYPE) continue;
				road_owner[rtt] = GetRoadOwner(cur_tile, rtt);
				/* If we don't want to preserve our roads then restore only roads of others. */
				if (remove_road && road_owner[rtt] == _current_company) road_type[rtt] = INVALID_ROADTYPE;
			}
			road_bits = AxisToRoadBits(GetDriveThroughStopAxis(cur_tile));
		}

		CommandCost ret;
		if (road_waypoint) {
			ret = RemoveRoadWaypointStop(cur_tile, flags);
		} else {
			ret = RemoveRoadStop(cur_tile, flags);
		}
		if (ret.Failed()) {
			last_error = std::move(ret);
			continue;
		}
		cost.AddCost(ret.GetCost());
		had_success = true;

		/* Restore roads. */
		if (flags.Test(DoCommandFlag::Execute) && (road_type[RTT_ROAD] != INVALID_ROADTYPE || road_type[RTT_TRAM] != INVALID_ROADTYPE)) {
			MakeRoadNormal(cur_tile, road_bits, road_type[RTT_ROAD], road_type[RTT_TRAM], ClosestTownFromTile(cur_tile, UINT_MAX)->index,
					road_owner[RTT_ROAD], road_owner[RTT_TRAM]);

			/* Update company infrastructure counts. */
			int count = CountBits(road_bits);
			UpdateCompanyRoadInfrastructure(road_type[RTT_ROAD], road_owner[RTT_ROAD], count);
			UpdateCompanyRoadInfrastructure(road_type[RTT_TRAM], road_owner[RTT_TRAM], count);
		}
	}

	return had_success ? cost : last_error;
}

/**
 * Remove bus or truck stops.
 * @param flags Operation to perform.
 * @param tile Northernmost tile of the removal area.
 * @param width Width of the removal area.
 * @param height Height of the removal area.
 * @param stop_type Type of stop (bus/truck).
 * @param remove_road Remove roads of drive-through stops?
 * @return The cost of this operation or an error.
 */
CommandCost CmdRemoveRoadStop(DoCommandFlags flags, TileIndex tile, uint8_t width, uint8_t height, RoadStopType stop_type, bool remove_road)
{
	if (stop_type >= RoadStopType::End) return CMD_ERROR;
	/* Check for incorrect width / height. */
	if (width == 0 || height == 0) return CMD_ERROR;
	/* Check if the first tile and the last tile are valid */
	if (!IsValidTile(tile) || TileAddWrap(tile, width - 1, height - 1) == INVALID_TILE) return CMD_ERROR;
	/* Bankrupting company is not supposed to remove roads, there may be road vehicles. */
	if (remove_road && flags.Test(DoCommandFlag::Bankrupt)) return CMD_ERROR;

	TileArea roadstop_area(tile, width, height);

	return RemoveGenericRoadStop(flags, roadstop_area, false, remove_road);
}

/**
 * Remove road waypoints.
 * @param flags operation to perform
 * @param start tile of road waypoint piece to remove
 * @param end other edge of the rect to remove
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveFromRoadWaypoint(DoCommandFlags flags, TileIndex start, TileIndex end)
{
	if (end == 0) end = start;
	if (start >= Map::Size() || end >= Map::Size()) return CMD_ERROR;

	TileArea roadstop_area(start, end);

	return RemoveGenericRoadStop(flags, roadstop_area, true, false);
}

/**
 * Get a possible noise reduction factor based on distance from town center.
 * The further you get, the less noise you generate.
 * So all those folks at city council can now happily slee...  work in their offices
 * @param as airport information
 * @param distance minimum distance between town and airport
 * @return the noise that will be generated, according to distance
 */
uint8_t GetAirportNoiseLevelForDistance(const AirportSpec *as, uint distance)
{
	/* 0 cannot be accounted, and 1 is the lowest that can be reduced from town.
	 * So no need to go any further*/
	if (as->noise_level < 2) return as->noise_level;

	/* The steps for measuring noise reduction are based on the "magical" (and arbitrary) 8 base distance
	 * adding the town_council_tolerance 4 times, as a way to graduate, depending of the tolerance.
	 * Basically, it says that the less tolerant a town is, the bigger the distance before
	 * an actual decrease can be granted */
	uint8_t town_tolerance_distance = 8 + (_settings_game.difficulty.town_council_tolerance * 4);

	/* now, we want to have the distance segmented using the distance judged bareable by town
	 * This will give us the coefficient of reduction the distance provides. */
	uint noise_reduction = distance / town_tolerance_distance;

	/* If the noise reduction equals the airport noise itself, don't give it for free.
	 * Otherwise, simply reduce the airport's level. */
	return noise_reduction >= as->noise_level ? 1 : as->noise_level - noise_reduction;
}

/**
 * Finds the town nearest to given airport. Based on minimal manhattan distance to any airport's tile.
 * If two towns have the same distance, town with lower index is returned.
 * @param as airport's description
 * @param rotation airport's rotation
 * @param tile origin tile (top corner of the airport)
 * @param it An iterator over all airport tiles (consumed)
 * @param[out] mindist Minimum distance to town
 * @return nearest town to airport
 */
Town *AirportGetNearestTown(const AirportSpec *as, Direction rotation, TileIndex tile, TileIterator &&it, uint &mindist)
{
	assert(Town::GetNumItems() > 0);

	Town *nearest = nullptr;

	auto width = as->size_x;
	auto height = as->size_y;
	if (rotation == DIR_E || rotation == DIR_W) std::swap(width, height);

	uint perimeter_min_x = TileX(tile);
	uint perimeter_min_y = TileY(tile);
	uint perimeter_max_x = perimeter_min_x + width - 1;
	uint perimeter_max_y = perimeter_min_y + height - 1;

	mindist = UINT_MAX - 1; // prevent overflow

	for (TileIndex cur_tile = *it; cur_tile != INVALID_TILE; cur_tile = ++it) {
		assert(IsInsideBS(TileX(cur_tile), perimeter_min_x, width));
		assert(IsInsideBS(TileY(cur_tile), perimeter_min_y, height));
		if (TileX(cur_tile) == perimeter_min_x || TileX(cur_tile) == perimeter_max_x || TileY(cur_tile) == perimeter_min_y || TileY(cur_tile) == perimeter_max_y) {
			Town *t = CalcClosestTownFromTile(cur_tile, mindist + 1);
			if (t == nullptr) continue;

			uint dist = DistanceManhattan(t->xy, cur_tile);
			if (dist == mindist && t->index < nearest->index) nearest = t;
			if (dist < mindist) {
				nearest = t;
				mindist = dist;
			}
		}
	}

	return nearest;
}

/**
 * Finds the town nearest to given existing airport. Based on minimal manhattan distance to any airport's tile.
 * If two towns have the same distance, town with lower index is returned.
 * @param station existing station with airport
 * @param[out] mindist Minimum distance to town
 * @return nearest town to airport
 */
static Town *AirportGetNearestTown(const Station *st, uint &mindist)
{
	return AirportGetNearestTown(st->airport.GetSpec(), st->airport.rotation, st->airport.tile, AirportTileIterator(st), mindist);
}


/** Recalculate the noise generated by the airports of each town */
void UpdateAirportsNoise()
{
	for (Town *t : Town::Iterate()) t->noise_reached = 0;

	for (const Station *st : Station::Iterate()) {
		if (st->airport.tile != INVALID_TILE && st->airport.type != AT_OILRIG) {
			uint dist;
			Town *nearest = AirportGetNearestTown(st, dist);
			nearest->noise_reached += GetAirportNoiseLevelForDistance(st->airport.GetSpec(), dist);
		}
	}
}

/**
 * Place an Airport.
 * @param flags operation to perform
 * @param tile tile where airport will be built
 * @param airport_type airport type, @see airport.h
 * @param layout airport layout
 * @param station_to_join station ID to join (NEW_STATION if build new one)
 * @param allow_adjacent allow airports directly adjacent to other airports.
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildAirport(DoCommandFlags flags, TileIndex tile, uint8_t airport_type, uint8_t layout, StationID station_to_join, bool allow_adjacent)
{
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (airport_type >= NUM_AIRPORTS) return CMD_ERROR;

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	/* Check if a valid, buildable airport was chosen for construction */
	const AirportSpec *as = AirportSpec::Get(airport_type);
	if (!as->IsAvailable() || layout >= as->layouts.size()) return CMD_ERROR;
	if (!as->IsWithinMapBounds(layout, tile)) return CMD_ERROR;

	Direction rotation = as->layouts[layout].rotation;
	int w = as->size_x;
	int h = as->size_y;
	if (rotation == DIR_E || rotation == DIR_W) std::swap(w, h);
	TileArea airport_area = TileArea(tile, w, h);

	if (w > _settings_game.station.station_spread || h > _settings_game.station.station_spread) {
		return CommandCost(STR_ERROR_STATION_TOO_SPREAD_OUT);
	}

	AirportTileTableIterator tile_iter(as->layouts[layout].tiles, tile);
	CommandCost cost = CheckFlatLandAirport(tile_iter, flags);
	if (cost.Failed()) return cost;

	/* The noise level is the noise from the airport and reduce it to account for the distance to the town center. */
	uint dist;
	Town *nearest = AirportGetNearestTown(as, rotation, tile, std::move(tile_iter), dist);
	uint newnoise_level = GetAirportNoiseLevelForDistance(as, dist);

	/* Check if local auth would allow a new airport */
	StringID authority_refuse_message = STR_NULL;
	Town *authority_refuse_town = nullptr;

	if (_settings_game.economy.station_noise_level) {
		/* do not allow to build a new airport if this raise the town noise over the maximum allowed by town */
		if ((nearest->noise_reached + newnoise_level) > nearest->MaxTownNoise()) {
			authority_refuse_message = STR_ERROR_LOCAL_AUTHORITY_REFUSES_NOISE;
			authority_refuse_town = nearest;
		}
	} else if (_settings_game.difficulty.town_council_tolerance != TOWN_COUNCIL_PERMISSIVE) {
		Town *t = ClosestTownFromTile(tile, UINT_MAX);
		uint num = 0;
		for (const Station *st : Station::Iterate()) {
			if (st->town == t && st->facilities.Test(StationFacility::Airport) && st->airport.type != AT_OILRIG) num++;
		}
		if (num >= 2) {
			authority_refuse_message = STR_ERROR_LOCAL_AUTHORITY_REFUSES_AIRPORT;
			authority_refuse_town = t;
		}
	}

	if (authority_refuse_message != STR_NULL) {
		return CommandCostWithParam(authority_refuse_message, authority_refuse_town->index);
	}

	Station *st = nullptr;
	ret = FindJoiningStation(StationID::Invalid(), station_to_join, allow_adjacent, airport_area, &st);
	if (ret.Failed()) return ret;

	/* Distant join */
	if (st == nullptr && distant_join) st = Station::GetIfValid(station_to_join);

	ret = BuildStationPart(&st, flags, reuse, airport_area, GetAirport(airport_type)->flags.Test(AirportFTAClass::Flag::Airplanes) ? STATIONNAMING_AIRPORT : STATIONNAMING_HELIPORT);
	if (ret.Failed()) return ret;

	if (st != nullptr && st->airport.tile != INVALID_TILE) {
		return CommandCost(STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT);
	}

	for (AirportTileTableIterator iter(as->layouts[layout].tiles, tile); iter != INVALID_TILE; ++iter) {
		cost.AddCost(_price[PR_BUILD_STATION_AIRPORT]);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Always add the noise, so there will be no need to recalculate when option toggles */
		nearest->noise_reached += newnoise_level;

		st->AddFacility(StationFacility::Airport, tile);
		st->airport.type = airport_type;
		st->airport.layout = layout;
		st->airport.blocks = {};
		st->airport.rotation = rotation;

		st->rect.BeforeAddRect(tile, w, h, StationRect::ADD_TRY);

		for (AirportTileTableIterator iter(as->layouts[layout].tiles, tile); iter != INVALID_TILE; ++iter) {
			Tile t(iter);
			MakeAirport(t, st->owner, st->index, iter.GetStationGfx(), WATER_CLASS_INVALID);
			SetStationTileRandomBits(t, GB(Random(), 0, 4));
			st->airport.Add(iter);

			if (AirportTileSpec::Get(GetTranslatedAirportTileID(iter.GetStationGfx()))->animation.status != AnimationStatus::NoAnimation) AddAnimatedTile(t);
		}

		/* Only call the animation trigger after all tiles have been built */
		for (AirportTileTableIterator iter(as->layouts[layout].tiles, tile); iter != INVALID_TILE; ++iter) {
			TriggerAirportTileAnimation(st, iter, AirportAnimationTrigger::Built);
		}

		UpdateAirplanesOnNewStation(st);

		Company::Get(st->owner)->infrastructure.airport++;

		st->AfterStationTileSetChange(true, StationType::Airport);
		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);

		if (_settings_game.economy.station_noise_level) {
			SetWindowDirty(WC_TOWN_VIEW, nearest->index);
		}
	}

	return cost;
}

/**
 * Remove an airport
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveAirport(TileIndex tile, DoCommandFlags flags)
{
	Station *st = Station::GetByTile(tile);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	tile = st->airport.tile;

	CommandCost cost(EXPENSES_CONSTRUCTION);

	for (const Aircraft *a : Aircraft::Iterate()) {
		if (!a->IsNormalAircraft()) continue;
		if (a->targetairport == st->index && a->state != FLYING) {
			return CommandCost(STR_ERROR_AIRCRAFT_IN_THE_WAY);
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		for (uint i = 0; i < st->airport.GetNumHangars(); ++i) {
			TileIndex tile_cur = st->airport.GetHangarTile(i);
			OrderBackup::Reset(tile_cur, false);
			CloseWindowById(WC_VEHICLE_DEPOT, tile_cur);
		}

		/* The noise level is the noise from the airport and reduce it to account for the distance to the town center.
		 * And as for construction, always remove it, even if the setting is not set, in order to avoid the
		 * need of recalculation */
		uint dist;
		Town *nearest = AirportGetNearestTown(st, dist);
		nearest->noise_reached -= GetAirportNoiseLevelForDistance(st->airport.GetSpec(), dist);

		if (_settings_game.economy.station_noise_level) {
			SetWindowDirty(WC_TOWN_VIEW, nearest->index);
		}
	}

	for (TileIndex tile_cur : st->airport) {
		if (!st->TileBelongsToAirport(tile_cur)) continue;

		CommandCost ret = EnsureNoVehicleOnGround(tile_cur);
		if (ret.Failed()) return ret;

		cost.AddCost(_price[PR_CLEAR_STATION_AIRPORT]);

		if (flags.Test(DoCommandFlag::Execute)) {
			DoClearSquare(tile_cur);
			DeleteNewGRFInspectWindow(GSF_AIRPORTTILES, tile_cur.base());
		}
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		/* Clear the persistent storage. */
		delete st->airport.psa;

		st->rect.AfterRemoveRect(st, st->airport);

		st->airport.Clear();
		st->facilities.Reset(StationFacility::Airport);
		SetWindowClassesDirty(WC_VEHICLE_ORDERS);

		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);

		Company::Get(st->owner)->infrastructure.airport--;

		st->AfterStationTileSetChange(false, StationType::Airport);

		DeleteNewGRFInspectWindow(GSF_AIRPORTS, st->index);
	}

	return cost;
}

/**
 * Open/close an airport to incoming aircraft.
 * @param flags Operation to perform.
 * @param station_id Station ID of the airport.
 * @return the cost of this operation or an error
 */
CommandCost CmdOpenCloseAirport(DoCommandFlags flags, StationID station_id)
{
	if (!Station::IsValidID(station_id)) return CMD_ERROR;
	Station *st = Station::Get(station_id);

	if (!st->facilities.Test(StationFacility::Airport) || st->owner == OWNER_NONE) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		st->airport.blocks.Flip(AirportBlock::AirportClosed);
		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_CLOSE_AIRPORT);
	}
	return CommandCost();
}

/**
 * Tests whether the company's vehicles have this station in orders
 * @param station station ID
 * @param include_company If true only check vehicles of \a company, if false only check vehicles of other companies
 * @param company company ID
 */
bool HasStationInUse(StationID station, bool include_company, CompanyID company)
{
	for (const OrderList *orderlist : OrderList::Iterate()) {
		const Vehicle *v = orderlist->GetFirstSharedVehicle();
		assert(v != nullptr);
		if ((v->owner == company) != include_company) continue;

		for (const Order &order : orderlist->GetOrders()) {
			if (order.GetDestination() == station && (order.IsType(OT_GOTO_STATION) || order.IsType(OT_GOTO_WAYPOINT))) {
				return true;
			}
		}
	}
	return false;
}

static const TileIndexDiffC _dock_tileoffs_chkaround[] = {
	{-1,  0},
	{ 0,  0},
	{ 0,  0},
	{ 0, -1}
};
static const uint8_t _dock_w_chk[4] = { 2, 1, 2, 1 };
static const uint8_t _dock_h_chk[4] = { 1, 2, 1, 2 };

/**
 * Build a dock/haven.
 * @param flags operation to perform
 * @param tile tile where dock will be built
 * @param station_to_join station ID to join (NEW_STATION if build new one)
 * @param adjacent allow docks directly adjacent to other docks.
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildDock(DoCommandFlags flags, TileIndex tile, StationID station_to_join, bool adjacent)
{
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = StationID::Invalid();
	bool distant_join = (station_to_join != StationID::Invalid());

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	DiagDirection direction = GetInclinedSlopeDirection(GetTileSlope(tile));
	if (direction == INVALID_DIAGDIR) return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	direction = ReverseDiagDir(direction);

	/* Docks cannot be placed on rapids */
	if (HasTileWaterGround(tile)) return CommandCost(STR_ERROR_SITE_UNSUITABLE);

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	if (IsBridgeAbove(tile)) return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);

	CommandCost cost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_STATION_DOCK]);
	ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
	if (ret.Failed()) return ret;
	cost.AddCost(ret.GetCost());

	TileIndex tile_cur = tile + TileOffsByDiagDir(direction);

	if (!HasTileWaterGround(tile_cur) || !IsTileFlat(tile_cur)) {
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}

	if (IsBridgeAbove(tile_cur)) return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);

	/* Get the water class of the water tile before it is cleared.*/
	WaterClass wc = GetWaterClass(tile_cur);

	bool add_cost = !IsWaterTile(tile_cur);
	ret = Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile_cur);
	if (ret.Failed()) return ret;
	if (add_cost) cost.AddCost(ret.GetCost());

	tile_cur += TileOffsByDiagDir(direction);
	if (!IsTileType(tile_cur, MP_WATER) || !IsTileFlat(tile_cur)) {
		return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}

	TileArea dock_area = TileArea(tile + ToTileIndexDiff(_dock_tileoffs_chkaround[direction]),
			_dock_w_chk[direction], _dock_h_chk[direction]);

	/* middle */
	Station *st = nullptr;
	ret = FindJoiningStation(StationID::Invalid(), station_to_join, adjacent, dock_area, &st);
	if (ret.Failed()) return ret;

	/* Distant join */
	if (st == nullptr && distant_join) st = Station::GetIfValid(station_to_join);

	ret = BuildStationPart(&st, flags, reuse, dock_area, STATIONNAMING_DOCK);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		st->ship_station.Add(tile);
		TileIndex flat_tile = tile + TileOffsByDiagDir(direction);
		st->ship_station.Add(flat_tile);
		st->AddFacility(StationFacility::Dock, tile);

		st->rect.BeforeAddRect(dock_area.tile, dock_area.w, dock_area.h, StationRect::ADD_TRY);

		/* If the water part of the dock is on a canal, update infrastructure counts.
		 * This is needed as we've cleared that tile before.
		 * Clearing object tiles may result in water tiles which are already accounted for in the water infrastructure total.
		 * See: MakeWaterKeepingClass() */
		if (wc == WATER_CLASS_CANAL && !(HasTileWaterClass(flat_tile) && GetWaterClass(flat_tile) == WATER_CLASS_CANAL && IsTileOwner(flat_tile, _current_company))) {
			Company::Get(st->owner)->infrastructure.water++;
		}
		Company::Get(st->owner)->infrastructure.station += 2;

		MakeDock(tile, st->owner, st->index, direction, wc);
		UpdateStationDockingTiles(st);

		st->AfterStationTileSetChange(true, StationType::Dock);
	}

	return cost;
}

void RemoveDockingTile(TileIndex t)
{
	for (DiagDirection d = DIAGDIR_BEGIN; d != DIAGDIR_END; d++) {
		TileIndex tile = t + TileOffsByDiagDir(d);
		if (!IsValidTile(tile)) continue;

		if (IsTileType(tile, MP_STATION)) {
			Station *st = Station::GetByTile(tile);
			if (st != nullptr) UpdateStationDockingTiles(st);
		} else if (IsTileType(tile, MP_INDUSTRY)) {
			Station *neutral = Industry::GetByTile(tile)->neutral_station;
			if (neutral != nullptr) UpdateStationDockingTiles(neutral);
		}
	}
}

/**
 * Clear docking tile status from tiles around a removed dock, if the tile has
 * no neighbours which would keep it as a docking tile.
 * @param tile Ex-dock tile to check.
 */
void ClearDockingTilesCheckingNeighbours(TileIndex tile)
{
	assert(IsValidTile(tile));

	/* Clear and maybe re-set docking tile */
	for (DiagDirection d = DIAGDIR_BEGIN; d != DIAGDIR_END; d++) {
		TileIndex docking_tile = tile + TileOffsByDiagDir(d);
		if (!IsValidTile(docking_tile)) continue;

		if (IsPossibleDockingTile(docking_tile)) {
			SetDockingTile(docking_tile, false);
			CheckForDockingTile(docking_tile);
		}
	}
}

/**
 * Find the part of a dock that is land-based
 * @param t Dock tile to find land part of
 * @return tile of land part of dock
 */
static TileIndex FindDockLandPart(TileIndex t)
{
	assert(IsDockTile(t));

	StationGfx gfx = GetStationGfx(t);
	if (gfx < GFX_DOCK_BASE_WATER_PART) return t;

	for (DiagDirection d = DIAGDIR_BEGIN; d != DIAGDIR_END; d++) {
		TileIndex tile = t + TileOffsByDiagDir(d);
		if (!IsValidTile(tile)) continue;
		if (!IsDockTile(tile)) continue;
		if (GetStationGfx(tile) < GFX_DOCK_BASE_WATER_PART && tile + TileOffsByDiagDir(GetDockDirection(tile)) == t) return tile;
	}

	return INVALID_TILE;
}

/**
 * Remove a dock
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveDock(TileIndex tile, DoCommandFlags flags)
{
	Station *st = Station::GetByTile(tile);
	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (!IsDockTile(tile)) return CMD_ERROR;

	TileIndex tile1 = FindDockLandPart(tile);
	if (tile1 == INVALID_TILE) return CMD_ERROR;
	TileIndex tile2 = tile1 + TileOffsByDiagDir(GetDockDirection(tile1));

	ret = EnsureNoVehicleOnGround(tile1);
	if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile2);
	if (ret.Failed()) return ret;

	if (flags.Test(DoCommandFlag::Execute)) {
		DoClearSquare(tile1);
		MarkTileDirtyByTile(tile1);
		MakeWaterKeepingClass(tile2, st->owner);

		st->rect.AfterRemoveTile(st, tile1);
		st->rect.AfterRemoveTile(st, tile2);

		MakeShipStationAreaSmaller(st);
		if (st->ship_station.tile == INVALID_TILE) {
			st->ship_station.Clear();
			st->docking_station.Clear();
			st->facilities.Reset(StationFacility::Dock);
			SetWindowClassesDirty(WC_VEHICLE_ORDERS);
		}

		Company::Get(st->owner)->infrastructure.station -= 2;

		st->AfterStationTileSetChange(false, StationType::Dock);

		ClearDockingTilesCheckingNeighbours(tile1);
		ClearDockingTilesCheckingNeighbours(tile2);

		for (Ship *s : Ship::Iterate()) {
			/* Find all ships going to our dock. */
			if (s->current_order.GetDestination() != st->index) {
				continue;
			}

			/* Find ships that are marked as "loading" but are no longer on a
			 * docking tile. Force them to leave the station (as they were loading
			 * on the removed dock). */
			if (s->current_order.IsType(OT_LOADING) && !(IsDockingTile(s->tile) && IsShipDestinationTile(s->tile, st->index))) {
				s->LeaveStation();
			}

			/* If we no longer have a dock, mark the order as invalid and send
			 * the ship to the next order (or, if there is none, make it
			 * wander the world). */
			if (s->current_order.IsType(OT_GOTO_STATION) && !st->facilities.Test(StationFacility::Dock)) {
				s->SetDestTile(s->GetOrderStationLocation(st->index));
			}
		}
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_STATION_DOCK]);
}

#include "table/station_land.h"

/**
 * Get station tile layout for a station type and its station gfx.
 * @param st Station type to draw.
 * @param gfx StationGfx of tile to draw.
 * @return Tile layout to draw.
 */
const DrawTileSprites *GetStationTileLayout(StationType st, uint8_t gfx)
{
	const auto &layouts = _station_display_datas[to_underlying(st)];
	if (gfx >= layouts.size()) gfx &= 1;
	return layouts.data() + gfx;
}

/**
 * Check whether a sprite is a track sprite, which can be replaced by a non-track ground sprite and a rail overlay.
 * If the ground sprite is suitable, \a ground is replaced with the new non-track ground sprite, and \a overlay_offset
 * is set to the overlay to draw.
 * @param         ti             Positional info for the tile to decide snowyness etc. May be nullptr.
 * @param[in,out] ground         Groundsprite to draw.
 * @param[out]    overlay_offset Overlay to draw.
 * @return true if overlay can be drawn.
 */
bool SplitGroundSpriteForOverlay(const TileInfo *ti, SpriteID *ground, RailTrackOffset *overlay_offset)
{
	bool snow_desert;
	switch (*ground) {
		case SPR_RAIL_TRACK_X:
		case SPR_MONO_TRACK_X:
		case SPR_MGLV_TRACK_X:
			snow_desert = false;
			*overlay_offset = RTO_X;
			break;

		case SPR_RAIL_TRACK_Y:
		case SPR_MONO_TRACK_Y:
		case SPR_MGLV_TRACK_Y:
			snow_desert = false;
			*overlay_offset = RTO_Y;
			break;

		case SPR_RAIL_TRACK_X_SNOW:
		case SPR_MONO_TRACK_X_SNOW:
		case SPR_MGLV_TRACK_X_SNOW:
			snow_desert = true;
			*overlay_offset = RTO_X;
			break;

		case SPR_RAIL_TRACK_Y_SNOW:
		case SPR_MONO_TRACK_Y_SNOW:
		case SPR_MGLV_TRACK_Y_SNOW:
			snow_desert = true;
			*overlay_offset = RTO_Y;
			break;

		default:
			return false;
	}

	if (ti != nullptr) {
		/* Decide snow/desert from tile */
		switch (_settings_game.game_creation.landscape) {
			case LandscapeType::Arctic:
				snow_desert = (uint)ti->z > GetSnowLine() * TILE_HEIGHT;
				break;

			case LandscapeType::Tropic:
				snow_desert = GetTropicZone(ti->tile) == TROPICZONE_DESERT;
				break;

			default:
				break;
		}
	}

	*ground = snow_desert ? SPR_FLAT_SNOW_DESERT_TILE : SPR_FLAT_GRASS_TILE;
	return true;
}

static void DrawTile_Station(TileInfo *ti)
{
	const NewGRFSpriteLayout *layout = nullptr;
	SpriteLayoutProcessor processor; // owns heap, borrowed by tmp_layout and t
	DrawTileSpriteSpan tmp_layout;
	const DrawTileSprites *t = nullptr;
	int32_t total_offset;
	const RailTypeInfo *rti = nullptr;
	uint32_t relocation = 0;
	uint32_t ground_relocation = 0;
	BaseStation *st = nullptr;
	const StationSpec *statspec = nullptr;
	uint tile_layout = 0;

	if (HasStationRail(ti->tile)) {
		rti = GetRailTypeInfo(GetRailType(ti->tile));
		total_offset = rti->GetRailtypeSpriteOffset();

		if (IsCustomStationSpecIndex(ti->tile)) {
			/* look for customization */
			st = BaseStation::GetByTile(ti->tile);
			statspec = st->speclist[GetCustomStationSpecIndex(ti->tile)].spec;

			if (statspec != nullptr) {
				tile_layout = GetStationGfx(ti->tile);

				if (statspec->callback_mask.Test(StationCallbackMask::DrawTileLayout)) {
					uint16_t callback = GetStationCallback(CBID_STATION_DRAW_TILE_LAYOUT, 0, 0, statspec, st, ti->tile);
					if (callback != CALLBACK_FAILED) tile_layout = (callback & ~1) + GetRailStationAxis(ti->tile);
				}

				/* Ensure the chosen tile layout is valid for this custom station */
				if (!statspec->renderdata.empty()) {
					layout = &statspec->renderdata[tile_layout < statspec->renderdata.size() ? tile_layout : (uint)GetRailStationAxis(ti->tile)];
					if (!layout->NeedsPreprocessing()) {
						t = layout;
						layout = nullptr;
					}
				}
			}
		}
	} else {
		total_offset = 0;
	}

	StationGfx gfx = GetStationGfx(ti->tile);
	if (IsAirport(ti->tile)) {
		gfx = GetAirportGfx(ti->tile);
		if (gfx >= NEW_AIRPORTTILE_OFFSET) {
			const AirportTileSpec *ats = AirportTileSpec::Get(gfx);
			if (ats->grf_prop.HasSpriteGroups() && DrawNewAirportTile(ti, Station::GetByTile(ti->tile), ats)) {
				return;
			}
			/* No sprite group (or no valid one) found, meaning no graphics associated.
			 * Use the substitute one instead */
			assert(ats->grf_prop.subst_id != INVALID_AIRPORTTILE);
			gfx = ats->grf_prop.subst_id;
		}
		switch (gfx) {
			case APT_RADAR_GRASS_FENCE_SW:
				t = &_station_display_datas_airport_radar_grass_fence_sw[GetAnimationFrame(ti->tile)];
				break;
			case APT_GRASS_FENCE_NE_FLAG:
				t = &_station_display_datas_airport_flag_grass_fence_ne[GetAnimationFrame(ti->tile)];
				break;
			case APT_RADAR_FENCE_SW:
				t = &_station_display_datas_airport_radar_fence_sw[GetAnimationFrame(ti->tile)];
				break;
			case APT_RADAR_FENCE_NE:
				t = &_station_display_datas_airport_radar_fence_ne[GetAnimationFrame(ti->tile)];
				break;
			case APT_GRASS_FENCE_NE_FLAG_2:
				t = &_station_display_datas_airport_flag_grass_fence_ne_2[GetAnimationFrame(ti->tile)];
				break;
		}
	}

	Owner owner = GetTileOwner(ti->tile);

	PaletteID palette;
	if (Company::IsValidID(owner)) {
		palette = GetCompanyPalette(owner);
	} else {
		/* Some stations are not owner by a company, namely oil rigs */
		palette = PALETTE_TO_GREY;
	}

	if (layout == nullptr && (t == nullptr || t->GetSequence().empty())) t = GetStationTileLayout(GetStationType(ti->tile), gfx);

	/* don't show foundation for docks */
	if (ti->tileh != SLOPE_FLAT && !IsDock(ti->tile)) {
		if (statspec != nullptr && statspec->flags.Test(StationSpecFlag::CustomFoundations)) {
			/* Station has custom foundations.
			 * Check whether the foundation continues beyond the tile's upper sides. */
			uint edge_info = 0;
			auto [slope, z] = GetFoundationPixelSlope(ti->tile);
			if (!HasFoundationNW(ti->tile, slope, z)) SetBit(edge_info, 0);
			if (!HasFoundationNE(ti->tile, slope, z)) SetBit(edge_info, 1);
			SpriteID image = GetCustomStationFoundationRelocation(statspec, st, ti->tile, tile_layout, edge_info);
			if (image == 0) goto draw_default_foundation;

			if (statspec->flags.Test(StationSpecFlag::ExtendedFoundations)) {
				/* Station provides extended foundations. */

				static const uint8_t foundation_parts[] = {
					0, 0, 0, 0, // Invalid,  Invalid,   Invalid,   SLOPE_SW
					0, 1, 2, 3, // Invalid,  SLOPE_EW,  SLOPE_SE,  SLOPE_WSE
					0, 4, 5, 6, // Invalid,  SLOPE_NW,  SLOPE_NS,  SLOPE_NWS
					7, 8, 9     // SLOPE_NE, SLOPE_ENW, SLOPE_SEN
				};

				AddSortableSpriteToDraw(image + foundation_parts[ti->tileh], PAL_NONE, *ti, {{}, {TILE_SIZE, TILE_SIZE, 7}, {}});
			} else {
				/* Draw simple foundations, built up from 8 possible foundation sprites. */

				/* Each set bit represents one of the eight composite sprites to be drawn.
				 * 'Invalid' entries will not drawn but are included for completeness. */
				static const uint8_t composite_foundation_parts[] = {
					/* Invalid  (00000000), Invalid   (11010001), Invalid   (11100100), SLOPE_SW  (11100000) */
					   0x00,                0xD1,                 0xE4,                 0xE0,
					/* Invalid  (11001010), SLOPE_EW  (11001001), SLOPE_SE  (11000100), SLOPE_WSE (11000000) */
					   0xCA,                0xC9,                 0xC4,                 0xC0,
					/* Invalid  (11010010), SLOPE_NW  (10010001), SLOPE_NS  (11100100), SLOPE_NWS (10100000) */
					   0xD2,                0x91,                 0xE4,                 0xA0,
					/* SLOPE_NE (01001010), SLOPE_ENW (00001001), SLOPE_SEN (01000100) */
					   0x4A,                0x09,                 0x44
				};

				uint8_t parts = composite_foundation_parts[ti->tileh];

				/* If foundations continue beyond the tile's upper sides then
				 * mask out the last two pieces. */
				if (HasBit(edge_info, 0)) ClrBit(parts, 6);
				if (HasBit(edge_info, 1)) ClrBit(parts, 7);

				if (parts == 0) {
					/* We always have to draw at least one sprite to make sure there is a boundingbox and a sprite with the
					 * correct offset for the childsprites.
					 * So, draw the (completely empty) sprite of the default foundations. */
					goto draw_default_foundation;
				}

				StartSpriteCombine();
				for (int i = 0; i < 8; i++) {
					if (HasBit(parts, i)) {
						AddSortableSpriteToDraw(image + i, PAL_NONE, *ti, {{}, {TILE_SIZE, TILE_SIZE, 7}, {}});
					}
				}
				EndSpriteCombine();
			}

			OffsetGroundSprite(0, -8);
			ti->z += ApplyPixelFoundationToSlope(FOUNDATION_LEVELED, ti->tileh);
		} else {
draw_default_foundation:
			DrawFoundation(ti, FOUNDATION_LEVELED);
		}
	}

	bool draw_ground = false;

	if (IsBuoy(ti->tile)) {
		DrawWaterClassGround(ti);
		SpriteID sprite = GetCanalSprite(CF_BUOY, ti->tile);
		if (sprite != 0) total_offset = sprite - SPR_IMG_BUOY;
	} else if (IsDock(ti->tile) || (IsOilRig(ti->tile) && IsTileOnWater(ti->tile))) {
		if (ti->tileh == SLOPE_FLAT) {
			DrawWaterClassGround(ti);
		} else {
			assert(IsDock(ti->tile));
			TileIndex water_tile = ti->tile + TileOffsByDiagDir(GetDockDirection(ti->tile));
			WaterClass wc = HasTileWaterClass(water_tile) ? GetWaterClass(water_tile) : WATER_CLASS_INVALID;
			if (wc == WATER_CLASS_SEA) {
				DrawShoreTile(ti->tileh);
			} else {
				DrawClearLandTile(ti, 3);
			}
		}
	} else if (IsRoadWaypointTile(ti->tile)) {
		RoadBits bits = AxisToRoadBits(GetDriveThroughStopAxis(ti->tile));
		RoadType road_rt = GetRoadTypeRoad(ti->tile);
		RoadType tram_rt = GetRoadTypeTram(ti->tile);
		RoadBits road = (road_rt != INVALID_ROADTYPE) ? bits : ROAD_NONE;
		RoadBits tram = (tram_rt != INVALID_ROADTYPE) ? bits : ROAD_NONE;
		const RoadTypeInfo *road_rti = (road_rt != INVALID_ROADTYPE) ? GetRoadTypeInfo(road_rt) : nullptr;
		const RoadTypeInfo *tram_rti = (tram_rt != INVALID_ROADTYPE) ? GetRoadTypeInfo(tram_rt) : nullptr;

		if (ti->tileh != SLOPE_FLAT) {
			DrawFoundation(ti, FOUNDATION_LEVELED);
		}

		DrawRoadGroundSprites(ti, road, tram, road_rti, tram_rti, GetRoadWaypointRoadside(ti->tile), IsRoadWaypointOnSnowOrDesert(ti->tile));
	} else {
		if (layout != nullptr) {
			/* Sprite layout which needs preprocessing */
			bool separate_ground = statspec->flags.Test(StationSpecFlag::SeparateGround);
			processor = SpriteLayoutProcessor(*layout, total_offset, rti->fallback_railtype, 0, 0, separate_ground);
			GetCustomStationRelocation(processor, statspec, st, ti->tile);
			tmp_layout = processor.GetLayout();
			t = &tmp_layout;
			total_offset = 0;
		} else if (statspec != nullptr) {
			/* Simple sprite layout */
			ground_relocation = relocation = GetCustomStationRelocation(statspec, st, ti->tile, 0);
			if (statspec->flags.Test(StationSpecFlag::SeparateGround)) {
				ground_relocation = GetCustomStationRelocation(statspec, st, ti->tile, 1);
			}
			ground_relocation += rti->fallback_railtype;
		}

		draw_ground = true;
	}

	if (draw_ground && !IsAnyRoadStop(ti->tile)) {
		SpriteID image = t->ground.sprite;
		PaletteID pal  = t->ground.pal;
		RailTrackOffset overlay_offset;
		if (rti != nullptr && rti->UsesOverlay() && SplitGroundSpriteForOverlay(ti, &image, &overlay_offset)) {
			SpriteID ground = GetCustomRailSprite(rti, ti->tile, RTSG_GROUND);
			DrawGroundSprite(image, PAL_NONE);
			DrawGroundSprite(ground + overlay_offset, PAL_NONE);

			if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation && HasStationReservation(ti->tile)) {
				SpriteID overlay = GetCustomRailSprite(rti, ti->tile, RTSG_OVERLAY);
				DrawGroundSprite(overlay + overlay_offset, PALETTE_CRASH);
			}
		} else {
			image += HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE) ? ground_relocation : total_offset;
			if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += ground_relocation;
			DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, palette));

			/* PBS debugging, draw reserved tracks darker */
			if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation && HasStationRail(ti->tile) && HasStationReservation(ti->tile)) {
				DrawGroundSprite(GetRailStationAxis(ti->tile) == AXIS_X ? rti->base_sprites.single_x : rti->base_sprites.single_y, PALETTE_CRASH);
			}
		}
	}

	if (HasStationRail(ti->tile) && HasRailCatenaryDrawn(GetRailType(ti->tile))) DrawRailCatenary(ti);

	if (IsAnyRoadStop(ti->tile)) {
		RoadType road_rt = GetRoadTypeRoad(ti->tile);
		RoadType tram_rt = GetRoadTypeTram(ti->tile);
		const RoadTypeInfo *road_rti = road_rt == INVALID_ROADTYPE ? nullptr : GetRoadTypeInfo(road_rt);
		const RoadTypeInfo *tram_rti = tram_rt == INVALID_ROADTYPE ? nullptr : GetRoadTypeInfo(tram_rt);

		StationGfx view = GetStationGfx(ti->tile);
		StationType type = GetStationType(ti->tile);

		const RoadStopSpec *stopspec = GetRoadStopSpec(ti->tile);
		RoadStopDrawModes stop_draw_mode{};
		if (stopspec != nullptr) {
			stop_draw_mode = stopspec->draw_mode;
			st = BaseStation::GetByTile(ti->tile);
			std::array<int32_t, 1> regs100;
			auto result = GetRoadStopLayout(ti, stopspec, st, type, view, regs100);
			if (result.has_value()) {
				if (stopspec->flags.Test(RoadStopSpecFlag::DrawModeRegister)) {
					stop_draw_mode = static_cast<RoadStopDrawModes>(regs100[0]);
				}
				if (type == StationType::RoadWaypoint && stop_draw_mode.Test(RoadStopDrawMode::WaypGround)) {
					draw_ground = true;
				}
				processor = std::move(*result);
				tmp_layout = processor.GetLayout();
				t = &tmp_layout;
			}
		}

		/* Draw ground sprite */
		if (draw_ground) {
			SpriteID image = t->ground.sprite;
			PaletteID pal  = t->ground.pal;
			image += HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE) ? ground_relocation : total_offset;
			if (GB(image, 0, SPRITE_WIDTH) != 0) {
				if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += ground_relocation;
				DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, palette));
			}
		}

		if (IsDriveThroughStopTile(ti->tile)) {
			if (type != StationType::RoadWaypoint && (stopspec == nullptr || stop_draw_mode.Test(RoadStopDrawMode::Overlay))) {
				uint sprite_offset = GetDriveThroughStopAxis(ti->tile) == AXIS_X ? 1 : 0;
				DrawRoadOverlays(ti, PAL_NONE, road_rti, tram_rti, sprite_offset, sprite_offset);
			}
		} else {
			/* Non-drivethrough road stops are only valid for roads. */
			assert(road_rt != INVALID_ROADTYPE && tram_rt == INVALID_ROADTYPE);

			if ((stopspec == nullptr || stop_draw_mode.Test(RoadStopDrawMode::Road)) && road_rti->UsesOverlay()) {
				SpriteID ground = GetCustomRoadSprite(road_rti, ti->tile, ROTSG_ROADSTOP);
				DrawGroundSprite(ground + view, PAL_NONE);
			}
		}

		if (stopspec == nullptr || !stopspec->flags.Test(RoadStopSpecFlag::NoCatenary)) {
			/* Draw road, tram catenary */
			DrawRoadCatenary(ti);
		}
	}

	if (IsRailWaypoint(ti->tile)) {
		/* Don't offset the waypoint graphics; they're always the same. */
		total_offset = 0;
	}

	DrawRailTileSeq(ti, t, TO_BUILDINGS, total_offset, relocation, palette);
}

void StationPickerDrawSprite(int x, int y, StationType st, RailType railtype, RoadType roadtype, int image)
{
	int32_t total_offset = 0;
	PaletteID pal = GetCompanyPalette(_local_company);
	const DrawTileSprites *t = GetStationTileLayout(st, image);
	const RailTypeInfo *railtype_info = nullptr;

	if (railtype != INVALID_RAILTYPE) {
		railtype_info = GetRailTypeInfo(railtype);
		total_offset = railtype_info->GetRailtypeSpriteOffset();
	}

	SpriteID img = t->ground.sprite;
	RailTrackOffset overlay_offset;
	if (railtype_info != nullptr && railtype_info->UsesOverlay() && SplitGroundSpriteForOverlay(nullptr, &img, &overlay_offset)) {
		SpriteID ground = GetCustomRailSprite(railtype_info, INVALID_TILE, RTSG_GROUND);
		DrawSprite(img, PAL_NONE, x, y);
		DrawSprite(ground + overlay_offset, PAL_NONE, x, y);
	} else {
		DrawSprite(img + total_offset, HasBit(img, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y);
	}

	if (roadtype != INVALID_ROADTYPE) {
		const RoadTypeInfo *roadtype_info = GetRoadTypeInfo(roadtype);
		if (image >= 4) {
			/* Drive-through stop */
			uint sprite_offset = 5 - image;

			/* Road underlay takes precedence over tram */
			if (roadtype_info->UsesOverlay()) {
				SpriteID ground = GetCustomRoadSprite(roadtype_info, INVALID_TILE, ROTSG_GROUND);
				DrawSprite(ground + sprite_offset, PAL_NONE, x, y);

				SpriteID overlay = GetCustomRoadSprite(roadtype_info, INVALID_TILE, ROTSG_OVERLAY);
				if (overlay) DrawSprite(overlay + sprite_offset, PAL_NONE, x, y);
			} else if (RoadTypeIsTram(roadtype)) {
				DrawSprite(SPR_TRAMWAY_TRAM + sprite_offset, PAL_NONE, x, y);
			}
		} else {
			/* Bay stop */
			if (RoadTypeIsRoad(roadtype) && roadtype_info->UsesOverlay()) {
				SpriteID ground = GetCustomRoadSprite(roadtype_info, INVALID_TILE, ROTSG_ROADSTOP);
				DrawSprite(ground + image, PAL_NONE, x, y);
			}
		}
	}

	/* Default waypoint has no railtype specific sprites */
	DrawRailTileSeqInGUI(x, y, t, (st == StationType::RailWaypoint || st == StationType::RoadWaypoint) ? 0 : total_offset, 0, pal);
}

static int GetSlopePixelZ_Station(TileIndex tile, uint, uint, bool)
{
	return GetTileMaxPixelZ(tile);
}

static Foundation GetFoundation_Station(TileIndex, Slope tileh)
{
	return FlatteningFoundation(tileh);
}

static void FillTileDescRoadStop(TileIndex tile, TileDesc &td)
{
	RoadType road_rt = GetRoadTypeRoad(tile);
	RoadType tram_rt = GetRoadTypeTram(tile);
	Owner road_owner = INVALID_OWNER;
	Owner tram_owner = INVALID_OWNER;
	if (road_rt != INVALID_ROADTYPE) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(road_rt);
		td.roadtype = rti->strings.name;
		td.road_speed = rti->max_speed / 2;
		road_owner = GetRoadOwner(tile, RTT_ROAD);
	}

	if (tram_rt != INVALID_ROADTYPE) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(tram_rt);
		td.tramtype = rti->strings.name;
		td.tram_speed = rti->max_speed / 2;
		tram_owner = GetRoadOwner(tile, RTT_TRAM);
	}

	if (IsDriveThroughStopTile(tile)) {
		/* Is there a mix of owners? */
		if ((tram_owner != INVALID_OWNER && tram_owner != td.owner[0]) ||
				(road_owner != INVALID_OWNER && road_owner != td.owner[0])) {
			uint i = 1;
			if (road_owner != INVALID_OWNER) {
				td.owner_type[i] = STR_LAND_AREA_INFORMATION_ROAD_OWNER;
				td.owner[i] = road_owner;
				i++;
			}
			if (tram_owner != INVALID_OWNER) {
				td.owner_type[i] = STR_LAND_AREA_INFORMATION_TRAM_OWNER;
				td.owner[i] = tram_owner;
			}
		}
	}
}

void FillTileDescRailStation(TileIndex tile, TileDesc &td)
{
	const StationSpec *spec = GetStationSpec(tile);

	if (spec != nullptr) {
		td.station_class = StationClass::Get(spec->class_index)->name;
		td.station_name  = spec->name;

		if (spec->grf_prop.HasGrfFile()) {
			const GRFConfig *gc = GetGRFConfig(spec->grf_prop.grfid);
			td.grf = gc->GetName();
		}
	}

	const RailTypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
	td.rail_speed = rti->max_speed;
	td.railtype = rti->strings.name;
}

void FillTileDescAirport(TileIndex tile, TileDesc &td)
{
	const AirportSpec *as = Station::GetByTile(tile)->airport.GetSpec();
	td.airport_class = AirportClass::Get(as->class_index)->name;
	td.airport_name = as->name;

	const AirportTileSpec *ats = AirportTileSpec::GetByTile(tile);
	td.airport_tile_name = ats->name;

	if (as->grf_prop.HasGrfFile()) {
		const GRFConfig *gc = GetGRFConfig(as->grf_prop.grfid);
		td.grf = gc->GetName();
	} else if (ats->grf_prop.HasGrfFile()) {
		const GRFConfig *gc = GetGRFConfig(ats->grf_prop.grfid);
		td.grf = gc->GetName();
	}
}

static void GetTileDesc_Station(TileIndex tile, TileDesc &td)
{
	td.owner[0] = GetTileOwner(tile);
	td.build_date = BaseStation::GetByTile(tile)->build_date;

	if (IsAnyRoadStop(tile)) FillTileDescRoadStop(tile, td);
	if (HasStationRail(tile)) FillTileDescRailStation(tile, td);
	if (IsAirport(tile)) FillTileDescAirport(tile, td);

	StringID str;
	switch (GetStationType(tile)) {
		default: NOT_REACHED();
		case StationType::Rail:     str = STR_LAI_STATION_DESCRIPTION_RAILROAD_STATION; break;
		case StationType::Airport:
			str = (IsHangar(tile) ? STR_LAI_STATION_DESCRIPTION_AIRCRAFT_HANGAR : STR_LAI_STATION_DESCRIPTION_AIRPORT);
			break;
		case StationType::Truck:    str = STR_LAI_STATION_DESCRIPTION_TRUCK_LOADING_AREA; break;
		case StationType::Bus:      str = STR_LAI_STATION_DESCRIPTION_BUS_STATION; break;
		case StationType::Oilrig: {
			const Industry *i = Station::GetByTile(tile)->industry;
			const IndustrySpec *is = GetIndustrySpec(i->type);
			td.owner[0] = i->owner;
			str = is->name;
			if (is->grf_prop.HasGrfFile()) td.grf = GetGRFConfig(is->grf_prop.grfid)->GetName();
			break;
		}
		case StationType::Dock:     str = STR_LAI_STATION_DESCRIPTION_SHIP_DOCK; break;
		case StationType::Buoy:     str = STR_LAI_STATION_DESCRIPTION_BUOY; break;
		case StationType::RailWaypoint: str = STR_LAI_STATION_DESCRIPTION_WAYPOINT; break;
		case StationType::RoadWaypoint: str = STR_LAI_STATION_DESCRIPTION_WAYPOINT; break;
	}
	td.str = str;
}


static TrackStatus GetTileTrackStatus_Station(TileIndex tile, TransportType mode, uint sub_mode, DiagDirection side)
{
	TrackBits trackbits = TRACK_BIT_NONE;

	switch (mode) {
		case TRANSPORT_RAIL:
			if (HasStationRail(tile) && !IsStationTileBlocked(tile)) {
				trackbits = TrackToTrackBits(GetRailStationTrack(tile));
			}
			break;

		case TRANSPORT_WATER:
			/* buoy is coded as a station, it is always on open water */
			if (IsBuoy(tile)) {
				trackbits = TRACK_BIT_ALL;
				/* remove tracks that connect NE map edge */
				if (TileX(tile) == 0) trackbits &= ~(TRACK_BIT_X | TRACK_BIT_UPPER | TRACK_BIT_RIGHT);
				/* remove tracks that connect NW map edge */
				if (TileY(tile) == 0) trackbits &= ~(TRACK_BIT_Y | TRACK_BIT_LEFT | TRACK_BIT_UPPER);
			}
			break;

		case TRANSPORT_ROAD:
			if (IsAnyRoadStop(tile)) {
				RoadTramType rtt = (RoadTramType)sub_mode;
				if (!HasTileRoadType(tile, rtt)) break;

				if (IsBayRoadStopTile(tile)) {
					DiagDirection dir = GetBayRoadStopDir(tile);
					if (side != INVALID_DIAGDIR && dir != side) break;
					trackbits = DiagDirToDiagTrackBits(dir);
				} else {
					Axis axis = GetDriveThroughStopAxis(tile);
					if (side != INVALID_DIAGDIR && axis != DiagDirToAxis(side)) break;
					trackbits = AxisToTrackBits(axis);
				}
			}
			break;

		default:
			break;
	}

	return CombineTrackStatus(TrackBitsToTrackdirBits(trackbits), TRACKDIR_BIT_NONE);
}


static void TileLoop_Station(TileIndex tile)
{
	auto *st = BaseStation::GetByTile(tile);
	switch (GetStationType(tile)) {
		case StationType::Airport:
			TriggerAirportTileAnimation(Station::From(st), tile, AirportAnimationTrigger::TileLoop);
			break;

		case StationType::Rail:
		case StationType::RailWaypoint:
			TriggerStationAnimation(st, tile, StationAnimationTrigger::TileLoop);
			break;

		case StationType::Dock:
			if (!IsTileFlat(tile)) break; // only handle water part
			[[fallthrough]];

		case StationType::Oilrig: //(station part)
		case StationType::Buoy:
			TileLoop_Water(tile);
			break;

		case StationType::Truck:
		case StationType::Bus:
			TriggerRoadStopAnimation(st, tile, StationAnimationTrigger::TileLoop);
			break;

		case StationType::RoadWaypoint: {
			switch (_settings_game.game_creation.landscape) {
				case LandscapeType::Arctic:
					if (IsRoadWaypointOnSnowOrDesert(tile) != (GetTileZ(tile) > GetSnowLine())) {
						ToggleRoadWaypointOnSnowOrDesert(tile);
						MarkTileDirtyByTile(tile);
					}
					break;

				case LandscapeType::Tropic:
					if (GetTropicZone(tile) == TROPICZONE_DESERT && !IsRoadWaypointOnSnowOrDesert(tile)) {
						ToggleRoadWaypointOnSnowOrDesert(tile);
						MarkTileDirtyByTile(tile);
					}
					break;

				default: break;
			}

			HouseZone new_zone = HouseZone::TownEdge;
			const Town *t = ClosestTownFromTile(tile, UINT_MAX);
			if (t != nullptr) {
				new_zone = GetTownRadiusGroup(t, tile);
			}

			/* Adjust road ground type depending on 'new_zone' */
			Roadside new_rs = new_zone != HouseZone::TownEdge ? ROADSIDE_PAVED : ROADSIDE_GRASS;
			Roadside cur_rs = GetRoadWaypointRoadside(tile);

			if (new_rs != cur_rs) {
				SetRoadWaypointRoadside(tile, cur_rs == ROADSIDE_BARREN ? new_rs : ROADSIDE_BARREN);
				MarkTileDirtyByTile(tile);
			}

			TriggerRoadStopAnimation(st, tile, StationAnimationTrigger::TileLoop);
			break;
		}

		default: break;
	}
}


static void AnimateTile_Station(TileIndex tile)
{
	if (HasStationRail(tile)) {
		AnimateStationTile(tile);
		return;
	}

	if (IsAirport(tile)) {
		AnimateAirportTile(tile);
		return;
	}

	if (IsAnyRoadStopTile(tile)) {
		AnimateRoadStopTile(tile);
		return;
	}
}


static bool ClickTile_Station(TileIndex tile)
{
	const BaseStation *bst = BaseStation::GetByTile(tile);

	if (bst->facilities.Test(StationFacility::Waypoint)) {
		ShowWaypointWindow(Waypoint::From(bst));
	} else if (IsHangar(tile)) {
		const Station *st = Station::From(bst);
		ShowDepotWindow(st->airport.GetHangarTile(st->airport.GetHangarNum(tile)), VEH_AIRCRAFT);
	} else {
		ShowStationViewWindow(bst->index);
	}
	return true;
}

static VehicleEnterTileStates VehicleEnter_Station(Vehicle *v, TileIndex tile, int x, int y)
{
	if (v->type == VEH_TRAIN) {
		StationID station_id = GetStationIndex(tile);
		if (!v->current_order.ShouldStopAtStation(v, station_id)) return {};
		if (!IsRailStation(tile) || !v->IsFrontEngine()) return {};

		int station_ahead;
		int station_length;
		int stop = GetTrainStopLocation(station_id, tile, Train::From(v), &station_ahead, &station_length);

		/* Stop whenever that amount of station ahead + the distance from the
		 * begin of the platform to the stop location is longer than the length
		 * of the platform. Station ahead 'includes' the current tile where the
		 * vehicle is on, so we need to subtract that. */
		if (stop + station_ahead - (int)TILE_SIZE >= station_length) return {};

		DiagDirection dir = DirToDiagDir(v->direction);

		x &= 0xF;
		y &= 0xF;

		if (DiagDirToAxis(dir) != AXIS_X) std::swap(x, y);
		if (y == TILE_SIZE / 2) {
			if (dir != DIAGDIR_SE && dir != DIAGDIR_SW) x = TILE_SIZE - 1 - x;
			stop &= TILE_SIZE - 1;

			if (x == stop) {
				return VehicleEnterTileState::EnteredStation; // enter station
			} else if (x < stop) {
				v->vehstatus.Set(VehState::TrainSlowing);
				uint16_t spd = std::max(0, (stop - x) * 20 - 15);
				if (spd < v->cur_speed) v->cur_speed = spd;
			}
		}
	} else if (v->type == VEH_ROAD) {
		RoadVehicle *rv = RoadVehicle::From(v);
		if (rv->state < RVSB_IN_ROAD_STOP && !IsReversingRoadTrackdir((Trackdir)rv->state) && rv->frame == 0) {
			if (IsStationRoadStop(tile) && rv->IsFrontEngine()) {
				/* Attempt to allocate a parking bay in a road stop */
				if (RoadStop::GetByTile(tile, GetRoadStopType(tile))->Enter(rv)) return {};
				return VehicleEnterTileState::CannotEnter;
			}
		}
	}

	return {};
}

/**
 * Run the watched cargo callback for all houses in the catchment area.
 * @param st Station.
 */
void TriggerWatchedCargoCallbacks(Station *st)
{
	/* Collect cargoes accepted since the last big tick. */
	CargoTypes cargoes = 0;
	for (CargoType cargo_type = 0; cargo_type < NUM_CARGO; cargo_type++) {
		if (st->goods[cargo_type].status.Test(GoodsEntry::State::AcceptedBigtick)) SetBit(cargoes, cargo_type);
	}

	/* Anything to do? */
	if (cargoes == 0) return;

	/* Loop over all houses in the catchment. */
	BitmapTileIterator it(st->catchment_tiles);
	for (TileIndex tile = it; tile != INVALID_TILE; tile = ++it) {
		if (IsTileType(tile, MP_HOUSE)) {
			TriggerHouseAnimation_WatchedCargoAccepted(tile, cargoes);
		}
	}
}

/**
 * This function is called for each station once every 250 ticks.
 * Not all stations will get the tick at the same time.
 * @param st the station receiving the tick.
 * @return true if the station is still valid (wasn't deleted)
 */
static bool StationHandleBigTick(BaseStation *st)
{
	if (!st->IsInUse()) {
		if (++st->delete_ctr >= 8) delete st;
		return false;
	}

	if (Station::IsExpected(st)) {
		TriggerWatchedCargoCallbacks(Station::From(st));

		for (GoodsEntry &ge : Station::From(st)->goods) {
			ge.status.Reset(GoodsEntry::State::AcceptedBigtick);
		}
	}


	if (!st->facilities.Test(StationFacility::Waypoint)) UpdateStationAcceptance(Station::From(st), true);

	return true;
}

static inline void byte_inc_sat(uint8_t *p)
{
	uint8_t b = *p + 1;
	if (b != 0) *p = b;
}

/**
 * Truncate the cargo by a specific amount.
 * @param cs The type of cargo to perform the truncation for.
 * @param ge The goods entry, of the station, to truncate.
 * @param amount The amount to truncate the cargo by.
 */
static void TruncateCargo(const CargoSpec *cs, GoodsEntry *ge, uint amount = UINT_MAX)
{
	/* If truncating also punish the source stations' ratings to
	 * decrease the flow of incoming cargo. */

	if (!ge->HasData()) return;

	StationCargoAmountMap waiting_per_source;
	ge->GetData().cargo.Truncate(amount, &waiting_per_source);
	for (StationCargoAmountMap::iterator i(waiting_per_source.begin()); i != waiting_per_source.end(); ++i) {
		Station *source_station = Station::GetIfValid(i->first);
		if (source_station == nullptr) continue;

		GoodsEntry &source_ge = source_station->goods[cs->Index()];
		source_ge.max_waiting_cargo = std::max(source_ge.max_waiting_cargo, i->second);
	}
}

static void UpdateStationRating(Station *st)
{
	bool waiting_changed = false;

	byte_inc_sat(&st->time_since_load);
	byte_inc_sat(&st->time_since_unload);

	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		GoodsEntry *ge = &st->goods[cs->Index()];
		/* Slowly increase the rating back to its original level in the case we
		 *  didn't deliver cargo yet to this station. This happens when a bribe
		 *  failed while you didn't moved that cargo yet to a station. */
		if (!ge->HasRating() && ge->rating < INITIAL_STATION_RATING) {
			ge->rating++;
		}

		/* Only change the rating if we are moving this cargo */
		if (ge->HasRating()) {
			byte_inc_sat(&ge->time_since_pickup);
			if (ge->time_since_pickup == 255 && _settings_game.order.selectgoods) {
				ge->status.Reset(GoodsEntry::State::Rating);
				ge->last_speed = 0;
				TruncateCargo(cs, ge);
				waiting_changed = true;
				continue;
			}

			bool skip = false;
			int rating = 0;
			uint waiting = ge->HasData() ? ge->GetData().cargo.AvailableCount() : 0;

			/* num_dests is at least 1 if there is any cargo as
			 * StationID::Invalid() is also a destination.
			 */
			uint num_dests = ge->HasData() ? static_cast<uint>(ge->GetData().cargo.Packets()->MapSize()) : 0;

			/* Average amount of cargo per next hop, but prefer solitary stations
			 * with only one or two next hops. They are allowed to have more
			 * cargo waiting per next hop.
			 * With manual cargo distribution waiting_avg = waiting / 2 as then
			 * StationID::Invalid() is the only destination.
			 */
			uint waiting_avg = waiting / (num_dests + 1);

			if (_cheats.station_rating.value) {
				ge->rating = rating = MAX_STATION_RATING;
				skip = true;
			} else if (cs->callback_mask.Test(CargoCallbackMask::StationRatingCalc)) {
				/* Perform custom station rating. If it succeeds the speed, days in transit and
				 * waiting cargo ratings must not be executed. */

				/* NewGRFs expect last speed to be 0xFF when no vehicle has arrived yet. */
				uint last_speed = ge->HasVehicleEverTriedLoading() ? ge->last_speed : 0xFF;

				uint32_t var18 = ClampTo<uint8_t>(ge->time_since_pickup)
					| (ClampTo<uint16_t>(ge->max_waiting_cargo) << 8)
					| (ClampTo<uint8_t>(last_speed) << 24);
				/* Convert to the 'old' vehicle types */
				uint32_t var10 = (st->last_vehicle_type == VEH_INVALID) ? 0x0 : (st->last_vehicle_type + 0x10);
				uint16_t callback = GetCargoCallback(CBID_CARGO_STATION_RATING_CALC, var10, var18, cs);
				if (callback != CALLBACK_FAILED) {
					skip = true;
					rating = GB(callback, 0, 14);

					/* Simulate a 15 bit signed value */
					if (HasBit(callback, 14)) rating -= 0x4000;
				}
			}

			if (!skip) {
				int b = ge->last_speed - 85;
				if (b >= 0) rating += b >> 2;

				uint8_t waittime = ge->time_since_pickup;
				if (st->last_vehicle_type == VEH_SHIP) waittime >>= 2;
				if (waittime <= 21) rating += 25;
				if (waittime <= 12) rating += 25;
				if (waittime <= 6) rating += 45;
				if (waittime <= 3) rating += 35;

				rating -= 90;
				if (ge->max_waiting_cargo <= 1500) rating += 55;
				if (ge->max_waiting_cargo <= 1000) rating += 35;
				if (ge->max_waiting_cargo <= 600) rating += 10;
				if (ge->max_waiting_cargo <= 300) rating += 20;
				if (ge->max_waiting_cargo <= 100) rating += 10;
			}

			if (Company::IsValidID(st->owner) && st->town->statues.Test(st->owner)) rating += 26;

			uint8_t age = ge->last_age;
			if (age < 3) rating += 10;
			if (age < 2) rating += 10;
			if (age < 1) rating += 13;

			{
				int or_ = ge->rating; // old rating

				/* only modify rating in steps of -2, -1, 0, 1 or 2 */
				ge->rating = rating = ClampTo<uint8_t>(or_ + Clamp(rating - or_, -2, 2));

				/* if rating is <= 64 and more than 100 items waiting on average per destination,
				 * remove some random amount of goods from the station */
				if (rating <= 64 && waiting_avg >= 100) {
					int dec = Random() & 0x1F;
					if (waiting_avg < 200) dec &= 7;
					waiting -= (dec + 1) * num_dests;
					waiting_changed = true;
				}

				/* if rating is <= 127 and there are any items waiting, maybe remove some goods. */
				if (rating <= 127 && waiting != 0) {
					uint32_t r = Random();
					if (rating <= (int)GB(r, 0, 7)) {
						/* Need to have int, otherwise it will just overflow etc. */
						waiting = std::max((int)waiting - (int)((GB(r, 8, 2) - 1) * num_dests), 0);
						waiting_changed = true;
					}
				}

				/* At some point we really must cap the cargo. Previously this
				 * was a strict 4095, but now we'll have a less strict, but
				 * increasingly aggressive truncation of the amount of cargo. */
				static const uint WAITING_CARGO_THRESHOLD  = 1 << 12;
				static const uint WAITING_CARGO_CUT_FACTOR = 1 <<  6;
				static const uint MAX_WAITING_CARGO        = 1 << 15;

				if (waiting > WAITING_CARGO_THRESHOLD) {
					uint difference = waiting - WAITING_CARGO_THRESHOLD;
					waiting -= (difference / WAITING_CARGO_CUT_FACTOR);

					waiting = std::min(waiting, MAX_WAITING_CARGO);
					waiting_changed = true;
				}

				/* We can't truncate cargo that's already reserved for loading.
				 * Thus StoredCount() here. */
				if (waiting_changed && waiting < (ge->HasData() ? ge->GetData().cargo.AvailableCount() : 0)) {
					/* Feed back the exact own waiting cargo at this station for the
					 * next rating calculation. */
					ge->max_waiting_cargo = 0;

					TruncateCargo(cs, ge, ge->GetData().cargo.AvailableCount() - waiting);
				} else {
					/* If the average number per next hop is low, be more forgiving. */
					ge->max_waiting_cargo = waiting_avg;
				}
			}
		}
	}

	StationID index = st->index;
	if (waiting_changed) {
		SetWindowDirty(WC_STATION_VIEW, index); // update whole window
	} else {
		SetWindowWidgetDirty(WC_STATION_VIEW, index, WID_SV_ACCEPT_RATING_LIST); // update only ratings list
	}
}

/**
 * Reroute cargo of type c at station st or in any vehicles unloading there.
 * Make sure the cargo's new next hop is neither "avoid" nor "avoid2".
 * @param st Station to be rerouted at.
 * @param cargo Type of cargo.
 * @param avoid Original next hop of cargo, avoid this.
 * @param avoid2 Another station to be avoided when rerouting.
 */
void RerouteCargo(Station *st, CargoType cargo, StationID avoid, StationID avoid2)
{
	GoodsEntry &ge = st->goods[cargo];

	/* Reroute cargo in station. */
	if (ge.HasData()) ge.GetData().cargo.Reroute(UINT_MAX, &ge.GetData().cargo, avoid, avoid2, &ge);

	/* Reroute cargo staged to be transferred. */
	for (Vehicle *v : st->loading_vehicles) {
		for (Vehicle *u = v; u != nullptr; u = u->Next()) {
			if (u->cargo_type != cargo) continue;
			u->cargo.Reroute(UINT_MAX, &u->cargo, avoid, avoid2, &ge);
		}
	}
}

/**
 * Check all next hops of cargo packets in this station for existence of a
 * a valid link they may use to travel on. Reroute any cargo not having a valid
 * link and remove timed out links found like this from the linkgraph. We're
 * not all links here as that is expensive and useless. A link no one is using
 * doesn't hurt either.
 * @param from Station to check.
 */
void DeleteStaleLinks(Station *from)
{
	for (CargoType cargo = 0; cargo < NUM_CARGO; ++cargo) {
		const bool auto_distributed = (_settings_game.linkgraph.GetDistributionType(cargo) != DT_MANUAL);
		GoodsEntry &ge = from->goods[cargo];
		LinkGraph *lg = LinkGraph::GetIfValid(ge.link_graph);
		if (lg == nullptr) continue;
		std::vector<NodeID> to_remove{};
		for (Edge &edge : (*lg)[ge.node].edges) {
			Station *to = Station::Get((*lg)[edge.dest_node].station);
			assert(to->goods[cargo].node == edge.dest_node);
			assert(TimerGameEconomy::date >= edge.LastUpdate());
			auto timeout = TimerGameEconomy::Date(LinkGraph::MIN_TIMEOUT_DISTANCE + (DistanceManhattan(from->xy, to->xy) >> 3));
			if (TimerGameEconomy::date - edge.LastUpdate() > timeout) {
				bool updated = false;

				if (auto_distributed) {
					/* Have all vehicles refresh their next hops before deciding to
					 * remove the node. */
					std::vector<Vehicle *> vehicles;
					for (const OrderList *l : OrderList::Iterate()) {
						bool found_from = false;
						bool found_to = false;
						for (const Order &order : l->GetOrders()) {
							if (!order.IsType(OT_GOTO_STATION) && !order.IsType(OT_IMPLICIT)) continue;
							if (order.GetDestination() == from->index) {
								found_from = true;
								if (found_to) break;
							} else if (order.GetDestination() == to->index) {
								found_to = true;
								if (found_from) break;
							}
						}
						if (!found_to || !found_from) continue;
						vehicles.push_back(l->GetFirstSharedVehicle());
					}

					auto iter = vehicles.begin();
					while (iter != vehicles.end()) {
						Vehicle *v = *iter;
						/* Do not refresh links of vehicles that have been stopped in depot for a long time. */
						if (!v->IsStoppedInDepot() || TimerGameEconomy::date - v->date_of_last_service <= LinkGraph::STALE_LINK_DEPOT_TIMEOUT) {
							LinkRefresher::Run(v, false); // Don't allow merging. Otherwise lg might get deleted.
						}
						if (edge.LastUpdate() == TimerGameEconomy::date) {
							updated = true;
							break;
						}

						Vehicle *next_shared = v->NextShared();
						if (next_shared) {
							*iter = next_shared;
							++iter;
						} else {
							iter = vehicles.erase(iter);
						}

						if (iter == vehicles.end()) iter = vehicles.begin();
					}
				}

				if (!updated) {
					/* If it's still considered dead remove it. */
					to_remove.emplace_back(to->goods[cargo].node);
					if (ge.HasData()) ge.GetData().flows.DeleteFlows(to->index);
					RerouteCargo(from, cargo, to->index, from->index);
				}
			} else if (edge.last_unrestricted_update != EconomyTime::INVALID_DATE && TimerGameEconomy::date - edge.last_unrestricted_update > timeout) {
				edge.Restrict();
				if (ge.HasData()) ge.GetData().flows.RestrictFlows(to->index);
				RerouteCargo(from, cargo, to->index, from->index);
			} else if (edge.last_restricted_update != EconomyTime::INVALID_DATE && TimerGameEconomy::date - edge.last_restricted_update > timeout) {
				edge.Release();
			}
		}
		/* Remove dead edges. */
		for (NodeID r : to_remove) (*lg)[ge.node].RemoveEdge(r);

		assert(TimerGameEconomy::date >= lg->LastCompression());
		if (TimerGameEconomy::date - lg->LastCompression() > LinkGraph::COMPRESSION_INTERVAL) {
			lg->Compress();
		}
	}
}

/**
 * Increase capacity for a link stat given by station cargo and next hop.
 * @param st Station to get the link stats from.
 * @param cargo Cargo to increase stat for.
 * @param next_station_id Station the consist will be travelling to next.
 * @param capacity Capacity to add to link stat.
 * @param usage Usage to add to link stat.
 * @param mode Update mode to be applied.
 */
void IncreaseStats(Station *st, CargoType cargo, StationID next_station_id, uint capacity, uint usage, uint32_t time, EdgeUpdateModes modes)
{
	GoodsEntry &ge1 = st->goods[cargo];
	Station *st2 = Station::Get(next_station_id);
	GoodsEntry &ge2 = st2->goods[cargo];
	LinkGraph *lg = nullptr;
	if (ge1.link_graph == LinkGraphID::Invalid()) {
		if (ge2.link_graph == LinkGraphID::Invalid()) {
			if (LinkGraph::CanAllocateItem()) {
				lg = new LinkGraph(cargo);
				LinkGraphSchedule::instance.Queue(lg);
				ge2.link_graph = lg->index;
				ge2.node = lg->AddNode(st2);
			} else {
				Debug(misc, 0, "Can't allocate link graph");
			}
		} else {
			lg = LinkGraph::Get(ge2.link_graph);
		}
		if (lg) {
			ge1.link_graph = lg->index;
			ge1.node = lg->AddNode(st);
		}
	} else if (ge2.link_graph == LinkGraphID::Invalid()) {
		lg = LinkGraph::Get(ge1.link_graph);
		ge2.link_graph = lg->index;
		ge2.node = lg->AddNode(st2);
	} else {
		lg = LinkGraph::Get(ge1.link_graph);
		if (ge1.link_graph != ge2.link_graph) {
			LinkGraph *lg2 = LinkGraph::Get(ge2.link_graph);
			if (lg->Size() < lg2->Size()) {
				LinkGraphSchedule::instance.Unqueue(lg);
				lg2->Merge(lg); // Updates GoodsEntries of lg
				lg = lg2;
			} else {
				LinkGraphSchedule::instance.Unqueue(lg2);
				lg->Merge(lg2); // Updates GoodsEntries of lg2
			}
		}
	}
	if (lg != nullptr) {
		(*lg)[ge1.node].UpdateEdge(ge2.node, capacity, usage, time, modes);
	}
}

/**
 * Increase capacity for all link stats associated with vehicles in the given consist.
 * @param st Station to get the link stats from.
 * @param front First vehicle in the consist.
 * @param next_station_id Station the consist will be travelling to next.
 */
void IncreaseStats(Station *st, const Vehicle *front, StationID next_station_id, uint32_t time)
{
	for (const Vehicle *v = front; v != nullptr; v = v->Next()) {
		if (v->refit_cap > 0) {
			/* The cargo count can indeed be higher than the refit_cap if
			 * wagons have been auto-replaced and subsequently auto-
			 * refitted to a higher capacity. The cargo gets redistributed
			 * among the wagons in that case.
			 * As usage is not such an important figure anyway we just
			 * ignore the additional cargo then.*/
			IncreaseStats(st, v->cargo_type, next_station_id, v->refit_cap,
					std::min<uint>(v->refit_cap, v->cargo.StoredCount()), time, EdgeUpdateMode::Increase);
		}
	}
}

/* called for every station each tick */
static void StationHandleSmallTick(BaseStation *st)
{
	if (st->facilities.Test(StationFacility::Waypoint) || !st->IsInUse()) return;

	uint8_t b = st->delete_ctr + 1;
	if (b >= Ticks::STATION_RATING_TICKS) b = 0;
	st->delete_ctr = b;

	if (b == 0) UpdateStationRating(Station::From(st));
}

void OnTick_Station()
{
	if (_game_mode == GM_EDITOR) return;

	for (BaseStation *st : BaseStation::Iterate()) {
		StationHandleSmallTick(st);

		/* Clean up the link graph about once a week. */
		if (Station::IsExpected(st) && (TimerGameTick::counter + st->index) % Ticks::STATION_LINKGRAPH_TICKS == 0) {
			DeleteStaleLinks(Station::From(st));
		};

		/* Spread out big-tick over STATION_ACCEPTANCE_TICKS ticks. */
		if ((TimerGameTick::counter + st->index) % Ticks::STATION_ACCEPTANCE_TICKS == 0) {
			/* Stop processing this station if it was deleted */
			if (!StationHandleBigTick(st)) continue;
		}

		/* Spread out station animation over STATION_ACCEPTANCE_TICKS ticks. */
		if ((TimerGameTick::counter + st->index) % Ticks::STATION_ACCEPTANCE_TICKS == 0) {
			TriggerStationAnimation(st, st->xy, StationAnimationTrigger::AcceptanceTick);
			TriggerRoadStopAnimation(st, st->xy, StationAnimationTrigger::AcceptanceTick);
			if (Station::IsExpected(st)) TriggerAirportAnimation(Station::From(st), AirportAnimationTrigger::AcceptanceTick);
		}
	}
}

/** Economy monthly loop for stations. */
static const IntervalTimer<TimerGameEconomy> _economy_stations_monthly({TimerGameEconomy::MONTH, TimerGameEconomy::Priority::STATION}, [](auto)
{
	for (Station *st : Station::Iterate()) {
		for (GoodsEntry &ge : st->goods) {
			ge.status.Set(GoodsEntry::State::LastMonth, ge.status.Test(GoodsEntry::State::CurrentMonth));
			ge.status.Reset(GoodsEntry::State::CurrentMonth);
		}
	}
});

void ModifyStationRatingAround(TileIndex tile, Owner owner, int amount, uint radius)
{
	ForAllStationsRadius(tile, radius, [&](Station *st) {
		if (st->owner == owner && DistanceManhattan(tile, st->xy) <= radius) {
			for (GoodsEntry &ge : st->goods) {
				if (ge.status.Any()) {
					ge.rating = ClampTo<uint8_t>(ge.rating + amount);
				}
			}
		}
	});
}

static uint UpdateStationWaiting(Station *st, CargoType cargo, uint amount, Source source)
{
	/* We can't allocate a CargoPacket? Then don't do anything
	 * at all; i.e. just discard the incoming cargo. */
	if (!CargoPacket::CanAllocateItem()) return 0;

	GoodsEntry &ge = st->goods[cargo];
	amount += ge.amount_fract;
	ge.amount_fract = GB(amount, 0, 8);

	amount >>= 8;
	/* No new "real" cargo item yet. */
	if (amount == 0) return 0;

	StationID next = ge.GetVia(st->index);
	ge.GetOrCreateData().cargo.Append(new CargoPacket(st->index, amount, source), next);
	LinkGraph *lg = nullptr;
	if (ge.link_graph == LinkGraphID::Invalid()) {
		if (LinkGraph::CanAllocateItem()) {
			lg = new LinkGraph(cargo);
			LinkGraphSchedule::instance.Queue(lg);
			ge.link_graph = lg->index;
			ge.node = lg->AddNode(st);
		} else {
			Debug(misc, 0, "Can't allocate link graph");
		}
	} else {
		lg = LinkGraph::Get(ge.link_graph);
	}
	if (lg != nullptr) (*lg)[ge.node].UpdateSupply(amount);

	if (!ge.HasRating()) {
		InvalidateWindowData(WC_STATION_LIST, st->owner);
		ge.status.Set(GoodsEntry::State::Rating);
	}

	TriggerStationRandomisation(st, st->xy, StationRandomTrigger::NewCargo, cargo);
	TriggerStationAnimation(st, st->xy, StationAnimationTrigger::NewCargo, cargo);
	TriggerAirportAnimation(st, AirportAnimationTrigger::NewCargo, cargo);
	TriggerRoadStopRandomisation(st, st->xy, StationRandomTrigger::NewCargo, cargo);
	TriggerRoadStopAnimation(st, st->xy, StationAnimationTrigger::NewCargo, cargo);


	SetWindowDirty(WC_STATION_VIEW, st->index);
	st->MarkTilesDirty(true);
	return amount;
}

static bool IsUniqueStationName(const std::string &name)
{
	for (const Station *st : Station::Iterate()) {
		if (!st->name.empty() && st->name == name) return false;
	}

	return true;
}

/**
 * Rename a station
 * @param flags operation to perform
 * @param station_id station ID that is to be renamed
 * @param text the new name or an empty string when resetting to the default
 * @return the cost of this operation or an error
 */
CommandCost CmdRenameStation(DoCommandFlags flags, StationID station_id, const std::string &text)
{
	Station *st = Station::GetIfValid(station_id);
	if (st == nullptr) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	bool reset = text.empty();

	if (!reset) {
		if (Utf8StringLength(text) >= MAX_LENGTH_STATION_NAME_CHARS) return CMD_ERROR;
		if (!IsUniqueStationName(text)) return CommandCost(STR_ERROR_NAME_MUST_BE_UNIQUE);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		st->cached_name.clear();
		if (reset) {
			st->name.clear();
		} else {
			st->name = text;
		}

		st->UpdateVirtCoord();
		InvalidateWindowData(WC_STATION_LIST, st->owner, 1);
	}

	return CommandCost();
}

static void AddNearbyStationsByCatchment(TileIndex tile, StationList &stations, StationList &nearby)
{
	for (Station *st : nearby) {
		if (st->TileIsInCatchment(tile)) stations.insert(st);
	}
}

/**
 * Run a tile loop to find stations around a tile, on demand. Cache the result for further requests
 * @return pointer to a StationList containing all stations found
 */
const StationList &StationFinder::GetStations()
{
	if (this->tile != INVALID_TILE) {
		if (IsTileType(this->tile, MP_HOUSE)) {
			/* Town nearby stations need to be filtered per tile. */
			assert(this->w == 1 && this->h == 1);
			AddNearbyStationsByCatchment(this->tile, this->stations, Town::GetByTile(this->tile)->stations_near);
		} else {
			ForAllStationsAroundTiles(*this, [this](Station *st, TileIndex) {
				this->stations.insert(st);
				return true;
			});
		}
		this->tile = INVALID_TILE;
	}
	return this->stations;
}


static bool CanMoveGoodsToStation(const Station *st, CargoType cargo)
{
	/* Is the station reserved exclusively for somebody else? */
	if (st->owner != OWNER_NONE && st->town->exclusive_counter > 0 && st->town->exclusivity != st->owner) return false;

	/* Lowest possible rating, better not to give cargo anymore. */
	if (st->goods[cargo].rating == 0) return false;

	/* Selectively servicing stations, and not this one. */
	if (_settings_game.order.selectgoods && !st->goods[cargo].HasVehicleEverTriedLoading()) return false;

	if (IsCargoInClass(cargo, CargoClass::Passengers)) {
		/* Passengers are never served by just a truck stop. */
		if (st->facilities == StationFacility::TruckStop) return false;
	} else {
		/* Non-passengers are never served by just a bus stop. */
		if (st->facilities == StationFacility::BusStop) return false;
	}
	return true;
}

uint MoveGoodsToStation(CargoType cargo, uint amount, Source source, const StationList &all_stations, Owner exclusivity)
{
	/* Return if nothing to do. Also the rounding below fails for 0. */
	if (all_stations.empty()) return 0;
	if (amount == 0) return 0;

	Station *first_station = nullptr;
	typedef std::pair<Station *, uint> StationInfo;
	std::vector<StationInfo> used_stations;

	for (Station *st : all_stations) {
		if (exclusivity != INVALID_OWNER && exclusivity != st->owner) continue;
		if (!CanMoveGoodsToStation(st, cargo)) continue;

		/* Avoid allocating a vector if there is only one station to significantly
		 * improve performance in this common case. */
		if (first_station == nullptr) {
			first_station = st;
			continue;
		}
		if (used_stations.empty()) {
			used_stations.reserve(2);
			used_stations.emplace_back(first_station, 0);
		}
		used_stations.emplace_back(st, 0);
	}

	/* no stations around at all? */
	if (first_station == nullptr) return 0;

	if (used_stations.empty()) {
		/* only one station around */
		amount *= first_station->goods[cargo].rating + 1;
		return UpdateStationWaiting(first_station, cargo, amount, source);
	}

	TypedIndexContainer<std::array<uint32_t, OWNER_END.base()>, Owner> company_best = {};  // best rating for each company, including OWNER_NONE
	TypedIndexContainer<std::array<uint32_t, OWNER_END.base()>, Owner> company_sum = {};   // sum of ratings for each company
	uint best_rating = 0;
	uint best_sum = 0;  // sum of best ratings for each company

	for (auto &p : used_stations) {
		auto owner = p.first->owner;
		auto rating = p.first->goods[cargo].rating;
		if (rating > company_best[owner]) {
			best_sum += rating - company_best[owner];  // it's usually faster than iterating companies later
			company_best[owner] = rating;
			if (rating > best_rating) best_rating = rating;
		}
		company_sum[owner] += rating;
	}

	/* From now we'll calculate with fractional cargo amounts.
	 * First determine how much cargo we really have. */
	amount *= best_rating + 1;

	uint moving = 0;
	for (auto &p : used_stations) {
		Owner owner = p.first->owner;
		/* Multiply the amount by (company best / sum of best for each company) to get cargo allocated to a company
		 * and by (station rating / sum of ratings in a company) to get the result for a single station. */
		p.second = amount * company_best[owner] * p.first->goods[cargo].rating / best_sum / company_sum[owner];
		moving += p.second;
	}

	/* If there is some cargo left due to rounding issues distribute it among the best rated stations. */
	if (amount > moving) {
		std::stable_sort(used_stations.begin(), used_stations.end(), [cargo](const StationInfo &a, const StationInfo &b) {
			return b.first->goods[cargo].rating < a.first->goods[cargo].rating;
		});

		assert(amount - moving <= used_stations.size());
		for (uint i = 0; i < amount - moving; i++) {
			used_stations[i].second++;
		}
	}

	uint moved = 0;
	for (auto &p : used_stations) {
		moved += UpdateStationWaiting(p.first, cargo, p.second, source);
	}

	return moved;
}

void UpdateStationDockingTiles(Station *st)
{
	st->docking_station.Clear();

	/* For neutral stations, start with the industry area instead of dock area */
	const TileArea *area = st->industry != nullptr ? &st->industry->location : &st->ship_station;

	if (area->tile == INVALID_TILE) return;

	int x = TileX(area->tile);
	int y = TileY(area->tile);

	/* Expand the area by a tile on each side while
	 * making sure that we remain inside the map. */
	int x2 = std::min<int>(x + area->w + 1, Map::SizeX());
	int x1 = std::max<int>(x - 1, 0);

	int y2 = std::min<int>(y + area->h + 1, Map::SizeY());
	int y1 = std::max<int>(y - 1, 0);

	TileArea ta(TileXY(x1, y1), TileXY(x2 - 1, y2 - 1));
	for (TileIndex tile : ta) {
		if (IsValidTile(tile) && IsPossibleDockingTile(tile)) CheckForDockingTile(tile);
	}
}

void BuildOilRig(TileIndex tile)
{
	if (!Station::CanAllocateItem()) {
		Debug(misc, 0, "Can't allocate station for oilrig at 0x{:X}, reverting to oilrig only", tile);
		return;
	}

	Station *st = new Station(tile);
	_station_kdtree.Insert(st->index);
	st->town = ClosestTownFromTile(tile, UINT_MAX);

	st->string_id = GenerateStationName(st, tile, STATIONNAMING_OILRIG);

	assert(IsTileType(tile, MP_INDUSTRY));
	/* Mark industry as associated both ways */
	st->industry = Industry::GetByTile(tile);
	st->industry->neutral_station = st;
	DeleteAnimatedTile(tile);
	MakeOilrig(tile, st->index, GetWaterClass(tile));

	st->owner = OWNER_NONE;
	st->airport.type = AT_OILRIG;
	st->airport.rotation = DIR_N;
	st->airport.Add(tile);
	st->ship_station.Add(tile);
	st->facilities = {StationFacility::Airport, StationFacility::Dock};
	st->build_date = TimerGameCalendar::date;
	UpdateStationDockingTiles(st);

	st->rect.BeforeAddTile(tile, StationRect::ADD_FORCE);

	st->UpdateVirtCoord();

	/* An industry tile has now been replaced with a station tile, this may change the overlap between station catchments and industry tiles.
	 * Recalculate the station catchment for all stations currently in the industry's nearby list.
	 * Clear the industry's station nearby list first because Station::RecomputeCatchment cannot remove nearby industries in this case. */
	if (_settings_game.station.serve_neutral_industries) {
		StationList nearby = std::move(st->industry->stations_near);
		st->industry->stations_near.clear();
		for (Station *near : nearby) {
			near->RecomputeCatchment(true);
			UpdateStationAcceptance(near, true);
		}
	}

	st->RecomputeCatchment();
	UpdateStationAcceptance(st, false);
}

void DeleteOilRig(TileIndex tile)
{
	Station *st = Station::GetByTile(tile);

	MakeWaterKeepingClass(tile, OWNER_NONE);

	/* The oil rig station is not supposed to be shared with anything else */
	[[maybe_unused]] static constexpr StationFacilities expected_facility{StationFacility::Airport, StationFacility::Dock};
	assert(st->facilities == expected_facility && st->airport.type == AT_OILRIG);
	if (st->industry != nullptr && st->industry->neutral_station == st) {
		/* Don't leave dangling neutral station pointer */
		st->industry->neutral_station = nullptr;
	}
	delete st;
}

static void ChangeTileOwner_Station(TileIndex tile, Owner old_owner, Owner new_owner)
{

	if (IsAnyRoadStopTile(tile)) {
		for (RoadTramType rtt : _roadtramtypes) {
			/* Update all roadtypes, no matter if they are present */
			if (GetRoadOwner(tile, rtt) == old_owner) {
				RoadType rt = GetRoadType(tile, rtt);
				if (rt != INVALID_ROADTYPE) {
					/* A drive-through road-stop has always two road bits. No need to dirty windows here, we'll redraw the whole screen anyway. */
					Company::Get(old_owner)->infrastructure.road[rt] -= 2;
					if (new_owner != INVALID_OWNER) Company::Get(new_owner)->infrastructure.road[rt] += 2;
				}
				SetRoadOwner(tile, rtt, new_owner == INVALID_OWNER ? OWNER_NONE : new_owner);
			}
		}
	}

	if (!IsTileOwner(tile, old_owner)) return;

	if (new_owner != INVALID_OWNER) {
		/* Update company infrastructure counts. Only do it here
		 * if the new owner is valid as otherwise the clear
		 * command will do it for us. No need to dirty windows
		 * here, we'll redraw the whole screen anyway.*/
		Company *old_company = Company::Get(old_owner);
		Company *new_company = Company::Get(new_owner);

		/* Update counts for underlying infrastructure. */
		switch (GetStationType(tile)) {
			case StationType::Rail:
			case StationType::RailWaypoint:
				if (!IsStationTileBlocked(tile)) {
					old_company->infrastructure.rail[GetRailType(tile)]--;
					new_company->infrastructure.rail[GetRailType(tile)]++;
				}
				break;

			case StationType::Bus:
			case StationType::Truck:
			case StationType::RoadWaypoint:
				/* Road stops were already handled above. */
				break;

			case StationType::Buoy:
			case StationType::Dock:
				if (GetWaterClass(tile) == WATER_CLASS_CANAL) {
					old_company->infrastructure.water--;
					new_company->infrastructure.water++;
				}
				break;

			default:
				break;
		}

		/* Update station tile count. */
		if (!IsBuoy(tile) && !IsAirport(tile)) {
			old_company->infrastructure.station--;
			new_company->infrastructure.station++;
		}

		/* for buoys, owner of tile is owner of water, st->owner == OWNER_NONE */
		SetTileOwner(tile, new_owner);
		InvalidateWindowClassesData(WC_STATION_LIST, 0);
	} else {
		if (IsDriveThroughStopTile(tile)) {
			/* Remove the drive-through road stop */
			if (IsRoadWaypoint(tile)) {
				Command<CMD_REMOVE_FROM_ROAD_WAYPOINT>::Do({DoCommandFlag::Execute, DoCommandFlag::Bankrupt}, tile, tile);
			} else {
				Command<CMD_REMOVE_ROAD_STOP>::Do({DoCommandFlag::Execute, DoCommandFlag::Bankrupt}, tile, 1, 1, (GetStationType(tile) == StationType::Truck) ? RoadStopType::Truck : RoadStopType::Bus, false);
			}
			assert(IsTileType(tile, MP_ROAD));
			/* Change owner of tile and all roadtypes */
			ChangeTileOwner(tile, old_owner, new_owner);
		} else {
			Command<CMD_LANDSCAPE_CLEAR>::Do({DoCommandFlag::Execute, DoCommandFlag::Bankrupt}, tile);
			/* Set tile owner of water under (now removed) buoy and dock to OWNER_NONE.
			 * Update owner of buoy if it was not removed (was in orders).
			 * Do not update when owned by OWNER_WATER (sea and rivers). */
			if ((IsTileType(tile, MP_WATER) || IsBuoyTile(tile)) && IsTileOwner(tile, old_owner)) SetTileOwner(tile, OWNER_NONE);
		}
	}
}

/**
 * Check if a drive-through road stop tile can be cleared.
 * Road stops built on town-owned roads check the conditions
 * that would allow clearing of the original road.
 * @param tile The road stop tile to check.
 * @param flags Command flags.
 * @return A succeeded command if the road can be removed, a failed command with the relevant error message otherwise.
 */
static CommandCost CanRemoveRoadWithStop(TileIndex tile, DoCommandFlags flags)
{
	/* Water flooding can always clear road stops. */
	if (_current_company == OWNER_WATER) return CommandCost();

	CommandCost ret;

	if (GetRoadTypeTram(tile) != INVALID_ROADTYPE) {
		Owner tram_owner = GetRoadOwner(tile, RTT_TRAM);
		if (tram_owner != OWNER_NONE) {
			ret = CheckOwnership(tram_owner);
			if (ret.Failed()) return ret;
		}
	}

	if (GetRoadTypeRoad(tile) != INVALID_ROADTYPE) {
		Owner road_owner = GetRoadOwner(tile, RTT_ROAD);
		if (road_owner == OWNER_TOWN) {
			ret = CheckAllowRemoveRoad(tile, GetAnyRoadBits(tile, RTT_ROAD), OWNER_TOWN, RTT_ROAD, flags);
			if (ret.Failed()) return ret;
		} else if (road_owner != OWNER_NONE) {
			ret = CheckOwnership(road_owner);
			if (ret.Failed()) return ret;
		}
	}

	return CommandCost();
}

/**
 * Clear a single tile of a station.
 * @param tile The tile to clear.
 * @param flags The DoCommand flags related to the "command".
 * @return The cost, or error of clearing.
 */
CommandCost ClearTile_Station(TileIndex tile, DoCommandFlags flags)
{
	if (flags.Test(DoCommandFlag::Auto)) {
		switch (GetStationType(tile)) {
			default: break;
			case StationType::Rail:     return CommandCost(STR_ERROR_MUST_DEMOLISH_RAILROAD);
			case StationType::RailWaypoint: return CommandCost(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
			case StationType::Airport:  return CommandCost(STR_ERROR_MUST_DEMOLISH_AIRPORT_FIRST);
			case StationType::Truck:    return CommandCost(HasTileRoadType(tile, RTT_TRAM) ? STR_ERROR_MUST_DEMOLISH_CARGO_TRAM_STATION_FIRST : STR_ERROR_MUST_DEMOLISH_TRUCK_STATION_FIRST);
			case StationType::Bus:      return CommandCost(HasTileRoadType(tile, RTT_TRAM) ? STR_ERROR_MUST_DEMOLISH_PASSENGER_TRAM_STATION_FIRST : STR_ERROR_MUST_DEMOLISH_BUS_STATION_FIRST);
			case StationType::RoadWaypoint: return CommandCost(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
			case StationType::Buoy:     return CommandCost(STR_ERROR_BUOY_IN_THE_WAY);
			case StationType::Dock:     return CommandCost(STR_ERROR_MUST_DEMOLISH_DOCK_FIRST);
			case StationType::Oilrig:
				return CommandCostWithParam(STR_ERROR_GENERIC_OBJECT_IN_THE_WAY, STR_INDUSTRY_NAME_OIL_RIG);
		}
	}

	switch (GetStationType(tile)) {
		case StationType::Rail:     return RemoveRailStation(tile, flags);
		case StationType::RailWaypoint: return RemoveRailWaypoint(tile, flags);
		case StationType::Airport:  return RemoveAirport(tile, flags);
		case StationType::Truck:    [[fallthrough]];
		case StationType::Bus:
			if (IsDriveThroughStopTile(tile)) {
				CommandCost remove_road = CanRemoveRoadWithStop(tile, flags);
				if (remove_road.Failed()) return remove_road;
			}
			return RemoveRoadStop(tile, flags);
		case StationType::RoadWaypoint: {
			CommandCost remove_road = CanRemoveRoadWithStop(tile, flags);
			if (remove_road.Failed()) return remove_road;
			return RemoveRoadWaypointStop(tile, flags);
		}
		case StationType::Buoy:     return RemoveBuoy(tile, flags);
		case StationType::Dock:     return RemoveDock(tile, flags);
		default: break;
	}

	return CMD_ERROR;
}

static CommandCost TerraformTile_Station(TileIndex tile, DoCommandFlags flags, int z_new, Slope tileh_new)
{
	if (_settings_game.construction.build_on_slopes && AutoslopeEnabled()) {
		/* TODO: If you implement newgrf callback 149 'land slope check', you have to decide what to do with it here.
		 *       TTDP does not call it.
		 */
		if (GetTileMaxZ(tile) == z_new + GetSlopeMaxZ(tileh_new)) {
			switch (GetStationType(tile)) {
				case StationType::RailWaypoint:
				case StationType::Rail: {
					if (!AutoslopeCheckForAxis(tile, z_new, tileh_new, GetRailStationAxis(tile))) break;
					return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
				}

				case StationType::Airport:
					return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);

				case StationType::Truck:
				case StationType::Bus:
				case StationType::RoadWaypoint: {
					if (IsDriveThroughStopTile(tile)) {
						if (!AutoslopeCheckForAxis(tile, z_new, tileh_new, GetDriveThroughStopAxis(tile))) break;
					} else {
						if (!AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, GetBayRoadStopDir(tile))) break;
					}
					return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
				}

				default: break;
			}
		}
	}
	return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
}

/**
 * Get flow for a station.
 * @param st Station to get flow for.
 * @return Flow for st.
 */
uint FlowStat::GetShare(StationID st) const
{
	uint32_t prev = 0;
	for (const auto &it : this->shares) {
		if (it.second == st) {
			return it.first - prev;
		} else {
			prev = it.first;
		}
	}
	return 0;
}

/**
 * Get a station a package can be routed to, but exclude the given ones.
 * @param excluded StationID not to be selected.
 * @param excluded2 Another StationID not to be selected.
 * @return A station ID from the shares map.
 */
StationID FlowStat::GetVia(StationID excluded, StationID excluded2) const
{
	if (this->unrestricted == 0) return StationID::Invalid();
	assert(!this->shares.empty());
	SharesMap::const_iterator it = this->shares.upper_bound(RandomRange(this->unrestricted));
	assert(it != this->shares.end() && it->first <= this->unrestricted);
	if (it->second != excluded && it->second != excluded2) return it->second;

	/* We've hit one of the excluded stations.
	 * Draw another share, from outside its range. */

	uint end = it->first;
	uint begin = (it == this->shares.begin() ? 0 : (--it)->first);
	uint interval = end - begin;
	if (interval >= this->unrestricted) return StationID::Invalid(); // Only one station in the map.
	uint new_max = this->unrestricted - interval;
	uint rand = RandomRange(new_max);
	SharesMap::const_iterator it2 = (rand < begin) ? this->shares.upper_bound(rand) :
			this->shares.upper_bound(rand + interval);
	assert(it2 != this->shares.end() && it2->first <= this->unrestricted);
	if (it2->second != excluded && it2->second != excluded2) return it2->second;

	/* We've hit the second excluded station.
	 * Same as before, only a bit more complicated. */

	uint end2 = it2->first;
	uint begin2 = (it2 == this->shares.begin() ? 0 : (--it2)->first);
	uint interval2 = end2 - begin2;
	if (interval2 >= new_max) return StationID::Invalid(); // Only the two excluded stations in the map.
	new_max -= interval2;
	if (begin > begin2) {
		std::swap(begin, begin2);
		std::swap(end, end2);
		std::swap(interval, interval2);
	}
	rand = RandomRange(new_max);
	SharesMap::const_iterator it3 = this->shares.upper_bound(this->unrestricted);
	if (rand < begin) {
		it3 = this->shares.upper_bound(rand);
	} else if (rand < begin2 - interval) {
		it3 = this->shares.upper_bound(rand + interval);
	} else {
		it3 = this->shares.upper_bound(rand + interval + interval2);
	}
	assert(it3 != this->shares.end() && it3->first <= this->unrestricted);
	return it3->second;
}

/**
 * Reduce all flows to minimum capacity so that they don't get in the way of
 * link usage statistics too much. Keep them around, though, to continue
 * routing any remaining cargo.
 */
void FlowStat::Invalidate()
{
	assert(!this->shares.empty());
	SharesMap new_shares;
	uint i = 0;
	for (const auto &it : this->shares) {
		new_shares[++i] = it.second;
		if (it.first == this->unrestricted) this->unrestricted = i;
	}
	this->shares.swap(new_shares);
	assert(!this->shares.empty() && this->unrestricted <= (--this->shares.end())->first);
}

/**
 * Change share for specified station. By specifying INT_MIN as parameter you
 * can erase a share. Newly added flows will be unrestricted.
 * @param st Next Hop to be removed.
 * @param flow Share to be added or removed.
 */
void FlowStat::ChangeShare(StationID st, int flow)
{
	/* We assert only before changing as afterwards the shares can actually
	 * be empty. In that case the whole flow stat must be deleted then. */
	assert(!this->shares.empty());

	uint removed_shares = 0;
	uint added_shares = 0;
	uint last_share = 0;
	SharesMap new_shares;
	for (const auto &it : this->shares) {
		if (it.second == st) {
			if (flow < 0) {
				uint share = it.first - last_share;
				if (flow == INT_MIN || (uint)(-flow) >= share) {
					removed_shares += share;
					if (it.first <= this->unrestricted) this->unrestricted -= share;
					if (flow != INT_MIN) flow += share;
					last_share = it.first;
					continue; // remove the whole share
				}
				removed_shares += (uint)(-flow);
			} else {
				added_shares += (uint)(flow);
			}
			if (it.first <= this->unrestricted) this->unrestricted += flow;

			/* If we don't continue above the whole flow has been added or
			 * removed. */
			flow = 0;
		}
		new_shares[it.first + added_shares - removed_shares] = it.second;
		last_share = it.first;
	}
	if (flow > 0) {
		new_shares[last_share + (uint)flow] = st;
		if (this->unrestricted < last_share) {
			this->ReleaseShare(st);
		} else {
			this->unrestricted += flow;
		}
	}
	this->shares.swap(new_shares);
}

/**
 * Restrict a flow by moving it to the end of the map and decreasing the amount
 * of unrestricted flow.
 * @param st Station of flow to be restricted.
 */
void FlowStat::RestrictShare(StationID st)
{
	assert(!this->shares.empty());
	uint flow = 0;
	uint last_share = 0;
	SharesMap new_shares;
	for (auto &it : this->shares) {
		if (flow == 0) {
			if (it.first > this->unrestricted) return; // Not present or already restricted.
			if (it.second == st) {
				flow = it.first - last_share;
				this->unrestricted -= flow;
			} else {
				new_shares[it.first] = it.second;
			}
		} else {
			new_shares[it.first - flow] = it.second;
		}
		last_share = it.first;
	}
	if (flow == 0) return;
	new_shares[last_share + flow] = st;
	this->shares.swap(new_shares);
	assert(!this->shares.empty());
}

/**
 * Release ("unrestrict") a flow by moving it to the begin of the map and
 * increasing the amount of unrestricted flow.
 * @param st Station of flow to be released.
 */
void FlowStat::ReleaseShare(StationID st)
{
	assert(!this->shares.empty());
	uint flow = 0;
	uint next_share = 0;
	bool found = false;
	for (SharesMap::reverse_iterator it(this->shares.rbegin()); it != this->shares.rend(); ++it) {
		if (it->first < this->unrestricted) return; // Note: not <= as the share may hit the limit.
		if (found) {
			flow = next_share - it->first;
			this->unrestricted += flow;
			break;
		} else {
			if (it->first == this->unrestricted) return; // !found -> Limit not hit.
			if (it->second == st) found = true;
		}
		next_share = it->first;
	}
	if (flow == 0) return;
	SharesMap new_shares;
	new_shares[flow] = st;
	for (SharesMap::iterator it(this->shares.begin()); it != this->shares.end(); ++it) {
		if (it->second != st) {
			new_shares[flow + it->first] = it->second;
		} else {
			flow = 0;
		}
	}
	this->shares.swap(new_shares);
	assert(!this->shares.empty());
}

/**
 * Scale all shares from link graph's runtime to monthly values.
 * @param runtime Time the link graph has been running without compression.
 * @pre runtime must be greater than 0 as we don't want infinite flow values.
 */
void FlowStat::ScaleToMonthly(uint runtime)
{
	assert(runtime > 0);
	SharesMap new_shares;
	uint share = 0;
	for (auto i : this->shares) {
		share = std::max(share + 1, i.first * 30 / runtime);
		new_shares[share] = i.second;
		if (this->unrestricted == i.first) this->unrestricted = share;
	}
	this->shares.swap(new_shares);
}

/**
 * Add some flow from "origin", going via "via".
 * @param origin Origin of the flow.
 * @param via Next hop.
 * @param flow Amount of flow to be added.
 */
void FlowStatMap::AddFlow(StationID origin, StationID via, uint flow)
{
	FlowStatMap::iterator origin_it = this->find(origin);
	if (origin_it == this->end()) {
		this->emplace(origin, FlowStat(via, flow));
	} else {
		origin_it->second.ChangeShare(via, flow);
		assert(!origin_it->second.GetShares()->empty());
	}
}

/**
 * Pass on some flow, remembering it as invalid, for later subtraction from
 * locally consumed flow. This is necessary because we can't have negative
 * flows and we don't want to sort the flows before adding them up.
 * @param origin Origin of the flow.
 * @param via Next hop.
 * @param flow Amount of flow to be passed.
 */
void FlowStatMap::PassOnFlow(StationID origin, StationID via, uint flow)
{
	FlowStatMap::iterator prev_it = this->find(origin);
	if (prev_it == this->end()) {
		FlowStat fs(via, flow);
		fs.AppendShare(StationID::Invalid(), flow);
		this->emplace(origin, fs);
	} else {
		prev_it->second.ChangeShare(via, flow);
		prev_it->second.ChangeShare(StationID::Invalid(), flow);
		assert(!prev_it->second.GetShares()->empty());
	}
}

/**
 * Subtract invalid flows from locally consumed flow.
 * @param self ID of own station.
 */
void FlowStatMap::FinalizeLocalConsumption(StationID self)
{
	for (auto &i : *this) {
		FlowStat &fs = i.second;
		uint local = fs.GetShare(StationID::Invalid());
		if (local > INT_MAX) { // make sure it fits in an int
			fs.ChangeShare(self, -INT_MAX);
			fs.ChangeShare(StationID::Invalid(), -INT_MAX);
			local -= INT_MAX;
		}
		fs.ChangeShare(self, -(int)local);
		fs.ChangeShare(StationID::Invalid(), -(int)local);

		/* If the local share is used up there must be a share for some
		 * remote station. */
		assert(!fs.GetShares()->empty());
	}
}

/**
 * Delete all flows at a station for specific cargo and destination.
 * @param via Remote station of flows to be deleted.
 * @return IDs of source stations for which the complete FlowStat, not only a
 *         share, has been erased.
 */
StationIDStack FlowStatMap::DeleteFlows(StationID via)
{
	StationIDStack ret;
	for (FlowStatMap::iterator f_it = this->begin(); f_it != this->end();) {
		FlowStat &s_flows = f_it->second;
		s_flows.ChangeShare(via, INT_MIN);
		if (s_flows.GetShares()->empty()) {
			ret.Push(f_it->first);
			this->erase(f_it++);
		} else {
			++f_it;
		}
	}
	return ret;
}

/**
 * Restrict all flows at a station for specific cargo and destination.
 * @param via Remote station of flows to be restricted.
 */
void FlowStatMap::RestrictFlows(StationID via)
{
	for (auto &it : *this) {
		it.second.RestrictShare(via);
	}
}

/**
 * Release all flows at a station for specific cargo and destination.
 * @param via Remote station of flows to be released.
 */
void FlowStatMap::ReleaseFlows(StationID via)
{
	for (auto &it : *this) {
		it.second.ReleaseShare(via);
	}
}

/**
 * Get the sum of all flows from this FlowStatMap.
 * @return sum of all flows.
 */
uint FlowStatMap::GetFlow() const
{
	uint ret = 0;
	for (const auto &it : *this) {
		ret += (--(it.second.GetShares()->end()))->first;
	}
	return ret;
}

/**
 * Get the sum of flows via a specific station from this FlowStatMap.
 * @param via Remote station to look for.
 * @return all flows for 'via' added up.
 */
uint FlowStatMap::GetFlowVia(StationID via) const
{
	uint ret = 0;
	for (const auto &it : *this) {
		ret += it.second.GetShare(via);
	}
	return ret;
}

/**
 * Get the sum of flows from a specific station from this FlowStatMap.
 * @param from Origin station to look for.
 * @return all flows from 'from' added up.
 */
uint FlowStatMap::GetFlowFrom(StationID from) const
{
	FlowStatMap::const_iterator i = this->find(from);
	if (i == this->end()) return 0;
	return (--(i->second.GetShares()->end()))->first;
}

/**
 * Get the flow from a specific station via a specific other station.
 * @param from Origin station to look for.
 * @param via Remote station to look for.
 * @return flow share originating at 'from' and going to 'via'.
 */
uint FlowStatMap::GetFlowFromVia(StationID from, StationID via) const
{
	FlowStatMap::const_iterator i = this->find(from);
	if (i == this->end()) return 0;
	return i->second.GetShare(via);
}

extern const TileTypeProcs _tile_type_station_procs = {
	DrawTile_Station,           // draw_tile_proc
	GetSlopePixelZ_Station,     // get_slope_z_proc
	ClearTile_Station,          // clear_tile_proc
	nullptr,                       // add_accepted_cargo_proc
	GetTileDesc_Station,        // get_tile_desc_proc
	GetTileTrackStatus_Station, // get_tile_track_status_proc
	ClickTile_Station,          // click_tile_proc
	AnimateTile_Station,        // animate_tile_proc
	TileLoop_Station,           // tile_loop_proc
	ChangeTileOwner_Station,    // change_tile_owner_proc
	nullptr,                       // add_produced_cargo_proc
	VehicleEnter_Station,       // vehicle_enter_tile_proc
	GetFoundation_Station,      // get_foundation_proc
	TerraformTile_Station,      // terraform_tile_proc
};
