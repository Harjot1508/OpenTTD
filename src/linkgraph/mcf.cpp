/** @file mcf.cpp Definition of Multi-Commodity-Flow solver. */

#include "../stdafx.h"
#include "../core/math_func.hpp"
#include "mcf.h"
#include <set>

typedef std::map<NodeID, Path *> PathViaMap;

/**
 * Distance-based annotation for use in the Dijkstra algorithm. This is close
 * to the original meaning of "annotation" in this context. Paths are rated
 * according to the sum of distances of their edges.
 */
class DistanceAnnotation : public Path {
public:

	/**
	 * Constructor.
	 * @param n ID of node to be annotated.
	 * @param source If the node is the source of its path.
	 */
	DistanceAnnotation(NodeID n, bool source = false) : Path(n, source) {}

	bool IsBetter(const DistanceAnnotation *base, uint cap, int free_cap, uint dist) const;

	/**
	 * Return the actual value of the annotation, in this case the distance.
	 * @return Distance.
	 */
	inline uint GetAnnotation() const { return this->distance; }

	/**
	 * Comparator for std containers.
	 */
	struct Comparator {
		bool operator()(const DistanceAnnotation *x, const DistanceAnnotation *y) const;
	};
};

/**
 * Capacity-based annotation for use in the Dijkstra algorithm. This annotation
 * rates paths according to the maximum capacity of their edges. The Dijkstra
 * algorithm still gives meaningful results like this as the capacity of a path
 * can only decrease or stay the same if you add more edges.
 */
class CapacityAnnotation : public Path {
public:

	/**
	 * Constructor.
	 * @param n ID of node to be annotated.
	 * @param source If the node is the source of its path.
	 */
	CapacityAnnotation(NodeID n, bool source = false) : Path(n, source) {}

	bool IsBetter(const CapacityAnnotation *base, uint cap, int free_cap, uint dist) const;

	/**
	 * Return the actual value of the annotation, in this case the capacity.
	 * @return Capacity.
	 */
	inline int GetAnnotation() const { return this->GetCapacityRatio(); }

	/**
	 * Comparator for std containers.
	 */
	struct Comparator {
		bool operator()(const CapacityAnnotation *x, const CapacityAnnotation *y) const;
	};
};

/**
 * Iterator class for getting the edges in the order of their next_edge
 * members.
 */
class GraphEdgeIterator {
private:
	LinkGraphJob &job; ///< Job being executed
	EdgeIterator i;    ///< Iterator pointing to current edge.
	EdgeIterator end;  ///< Iterator pointing beyond last edge.

public:

	/**
	 * Construct a GraphEdgeIterator.
	 * @param job Job to iterate on.
	 */
	GraphEdgeIterator(LinkGraphJob &job) : job(job),
		i(NULL, NULL, INVALID_NODE), end(NULL, NULL, INVALID_NODE)
	{}

	/**
	 * Setup the node to start iterating at.
	 * @param source Unused.
	 * @param node Node to start iterating at.
	 */
	void SetNode(NodeID source, NodeID node)
	{
		this->i = this->job[node].Begin();
		this->end = this->job[node].End();
	}

	/**
	 * Retrieve the ID of the node the next edge points to.
	 * @return Next edge's target node ID or INVALID_NODE.
	 */
	NodeID Next()
	{
		return this->i != this->end ? (this->i++)->first : INVALID_NODE;
	}
};

/**
 * Iterator class for getting edges from a FlowStatMap.
 */
class FlowEdgeIterator {
private:
	LinkGraphJob &job; ///< Link graph job we're working with.

	/** Lookup table for getting NodeIDs from StationIDs. */
	std::map<StationID, NodeID> station_to_node;

	/** Current iterator in the shares map. */
	FlowStat::SharesMap::const_iterator it;

	/** End of the shares map. */
	FlowStat::SharesMap::const_iterator end;
public:

	/**
	 * Constructor.
	 * @param job Link graph job to work with.
	 */
	FlowEdgeIterator(LinkGraphJob &job) : job(job)
	{
		for (NodeID i = 0; i < job.Size(); ++i) {
			this->station_to_node[job[i].Station()] = i;
		}
	}

	/**
	 * Setup the node to retrieve edges from.
	 * @param source Root of the current path tree.
	 * @param node Current node to be checked for outgoing flows.
	 */
	void SetNode(NodeID source, NodeID node)
	{
		static const FlowStat::SharesMap empty;
		const FlowStatMap &flows = this->job[node].Flows();
		FlowStatMap::const_iterator it = flows.find(this->job[source].Station());
		if (it != flows.end()) {
			this->it = it->second.GetShares()->begin();
			this->end = it->second.GetShares()->end();
		} else {
			this->it = empty.begin();
			this->end = empty.end();
		}
	}

	/**
	 * Get the next node for which a flow exists.
	 * @return ID of next node with flow.
	 */
	NodeID Next()
	{
		if (this->it == this->end) return INVALID_NODE;
		return this->station_to_node[(this->it++)->second];
	}
};

/**
 * Determines if an extension to the given Path with the given parameters is
 * better than this path.
 * @param base Other path.
 * @param cap Capacity of the new edge to be added to base.
 * @param dist Distance of the new edge.
 * @return True if base + the new edge would be better than the path associated
 * with this annotation.
 */
bool DistanceAnnotation::IsBetter(const DistanceAnnotation *base, uint cap,
		int free_cap, uint dist) const
{
	/* If any of the paths is disconnected, the other one is better. If both
	 * are disconnected, this path is better.*/
	if (base->distance == UINT_MAX) {
		return false;
	} else if (this->distance == UINT_MAX) {
		return true;
	}

	if (free_cap > 0 && base->free_capacity > 0) {
		/* If both paths have capacity left, compare their distances.
		 * If the other path has capacity left and this one hasn't, the
		 * other one's better (thus, return true). */
		return this->free_capacity > 0 ? (base->distance + dist < this->distance) : true;
	} else {
		/* If the other path doesn't have capacity left, but this one has,
		 * the other one is worse (thus, return false).
		 * If both paths are out of capacity, do the regular distance
		 * comparison. */
		return this->free_capacity > 0 ? false : (base->distance + dist < this->distance);
	}
}

/**
 * Determines if an extension to the given Path with the given parameters is
 * better than this path.
 * @param base Other path.
 * @param cap Capacity of the new edge to be added to base.
 * @param dist Distance of the new edge.
 * @return True if base + the new edge would be better than the path associated
 * with this annotation.
 */
bool CapacityAnnotation::IsBetter(const CapacityAnnotation *base, uint cap,
		int free_cap, uint dist) const
{
	int min_cap = Path::GetCapacityRatio(min(base->free_capacity, free_cap), min(base->capacity, cap));
	int this_cap = this->GetCapacityRatio();
	if (min_cap == this_cap) {
		/* If the capacities are the same and the other path isn't disconnected
		 * choose the shorter path. */
		return base->distance == UINT_MAX ? false : (base->distance + dist < this->distance);
	} else {
		return min_cap > this_cap;
	}
}

/**
 * A slightly modified Dijkstra algorithm. Grades the paths not necessarily by
 * distance, but by the value Tannotation computes. It uses the max_saturation
 * setting to artificially decrease capacities.
 * @tparam Tannotation Annotation to be used.
 * @tparam Tedge_iterator Iterator to be used for getting outgoing edges.
 * @param source_node Node where the algorithm starts.
 * @param paths Container for the paths to be calculated.
 */
template<class Tannotation, class Tedge_iterator>
void MultiCommodityFlow::Dijkstra(NodeID source_node, PathVector &paths)
{
	typedef std::set<Tannotation *, typename Tannotation::Comparator> AnnoSet;
	Tedge_iterator iter(this->job);
	uint size = this->job.Size();
	AnnoSet annos;
	paths.resize(size, NULL);
	for (NodeID node = 0; node < size; ++node) {
		Tannotation *anno = new Tannotation(node, node == source_node);
		annos.insert(anno);
		paths[node] = anno;
	}
	while (!annos.empty()) {
		typename AnnoSet::iterator i = annos.begin();
		Tannotation *source = *i;
		annos.erase(i);
		NodeID from = source->GetNode();
		iter.SetNode(source_node, from);
		for (NodeID to = iter.Next(); to != INVALID_NODE; to = iter.Next()) {
			if (to == from) continue; // Not a real edge but a consumption sign.
			Edge edge = this->job[from][to];
			assert(edge.Distance() < UINT_MAX);
			uint capacity = edge.Capacity();
			if (this->max_saturation != UINT_MAX) {
				capacity *= this->max_saturation;
				capacity /= 100;
				if (capacity == 0) capacity = 1;
			}
			/* punish in-between stops a little */
			uint distance = edge.Distance() + 1;
			Tannotation *dest = static_cast<Tannotation *>(paths[to]);
			if (dest->IsBetter(source, capacity, capacity - edge.Flow(), distance)) {
				annos.erase(dest);
				dest->Fork(source, capacity, capacity - edge.Flow(), distance);
				annos.insert(dest);
			}
		}
	}
}

/**
 * Clean up paths that lead nowhere and the root path.
 * @param source_id ID of the root node.
 * @param paths Paths to be cleaned up.
 */
void MultiCommodityFlow::CleanupPaths(NodeID source_id, PathVector &paths)
{
	Path *source = paths[source_id];
	paths[source_id] = NULL;
	for (PathVector::iterator i = paths.begin(); i != paths.end(); ++i) {
		Path *path = *i;
		if (path == NULL) continue;
		if (path->GetParent() == source) path->Detach();
		while (path != source && path != NULL && path->GetFlow() == 0) {
			Path *parent = path->GetParent();
			path->Detach();
			if (path->GetNumChildren() == 0) {
				paths[path->GetNode()] = NULL;
				delete path;
			}
			path = parent;
		}
	}
	delete source;
	paths.clear();
}

/**
 * Push flow along a path and update the unsatisfied_demand of the associated
 * edge.
 * @param edge Edge whose ends the path connects.
 * @param path End of the path the flow should be pushed on.
 * @param accuracy Accuracy of the calculation.
 * @param max_saturation If < UINT_MAX only push flow up to the given
 * 	                     saturation, otherwise the path can be "overloaded".
 */
uint MultiCommodityFlow::PushFlow(Edge &edge, Path *path, uint accuracy,
		uint max_saturation)
{
	assert(edge.UnsatisfiedDemand() > 0);
	uint flow = Clamp(edge.Demand() / accuracy, 1, edge.UnsatisfiedDemand());
	flow = path->AddFlow(flow, this->job, max_saturation);
	edge.SatisfyDemand(flow);
	return flow;
}

/**
 * Find the flow along a cycle including cycle_begin in path.
 * @param path Set of paths that form the cycle.
 * @param cycle_begin Path to start at.
 * @return Flow along the cycle.
 */
uint MCF1stPass::FindCycleFlow(const PathVector &path, const Path *cycle_begin)
{
	uint flow = UINT_MAX;
	const Path *cycle_end = cycle_begin;
	do {
		flow = min(flow, cycle_begin->GetFlow());
		cycle_begin = path[cycle_begin->GetNode()];
	} while (cycle_begin != cycle_end);
	return flow;
}

/**
 * Eliminate a cycle of the given flow in the given set of paths.
 * @param path Set of paths containing the cycle.
 * @param cycle_begin Part of the cycle to start at.
 * @param flow Flow along the cycle.
 */
void MCF1stPass::EliminateCycle(PathVector &path, Path *cycle_begin, uint flow)
{
	Path *cycle_end = cycle_begin;
	do {
		NodeID prev = cycle_begin->GetNode();
		cycle_begin->ReduceFlow(flow);
		cycle_begin = path[cycle_begin->GetNode()];
		Edge edge = this->job[prev][cycle_begin->GetNode()];
		edge.RemoveFlow(flow);
	} while (cycle_begin != cycle_end);
}

/**
 * Eliminate cycles for origin_id in the graph. Start searching at next_id and
 * work recursively. Also "summarize" paths: Add up the flows along parallel
 * paths in one.
 * @param path Paths checked in parent calls to this method.
 * @param origin_id Origin of the paths to be checked.
 * @param next_id Next node to be checked.
 * @return If any cycles have been found and eliminated.
 */
bool MCF1stPass::EliminateCycles(PathVector &path, NodeID origin_id, NodeID next_id)
{
	static Path *invalid_path = new Path(INVALID_NODE, true);
	Path *at_next_pos = path[next_id];

	/* this node has already been searched */
	if (at_next_pos == invalid_path) return false;

	if (at_next_pos == NULL) {
		/* Summarize paths; add up the paths with the same source and next hop
		 * in one path each. */
		PathList &paths = this->job[next_id].Paths();
		PathViaMap next_hops;
		for (PathList::iterator i = paths.begin(); i != paths.end(); ++i) {
			Path *new_child = *i;
			if (new_child->GetOrigin() == origin_id) {
				PathViaMap::iterator via_it = next_hops.find(new_child->GetNode());
				if (via_it == next_hops.end()) {
					next_hops[new_child->GetNode()] = new_child;
				} else {
					Path *child = via_it->second;
					uint new_flow = new_child->GetFlow();
					child->AddFlow(new_flow);
					new_child->ReduceFlow(new_flow);
				}
			}
		}
		bool found = false;
		/* Search the next hops for nodes we have already visited */
		for (PathViaMap::iterator via_it = next_hops.begin();
				via_it != next_hops.end(); ++via_it) {
			Path *child = via_it->second;
			if (child->GetFlow() > 0) {
				/* Push one child into the path vector and search this child's
				 * children. */
				path[next_id] = child;
				found = this->EliminateCycles(path, origin_id, child->GetNode()) || found;
			}
		}
		/* All paths departing from this node have been searched. Mark as
		 * resolved if no cycles found. If cycles were found further cycles
		 * could be found in this branch, thus it has to be searched again next
		 * time we spot it.
		 */
		path[next_id] = found ? NULL : invalid_path;
		return found;
	}

	/* This node has already been visited => we have a cycle.
	 * Backtrack to find the exact flow. */
	uint flow = this->FindCycleFlow(path, at_next_pos);
	if (flow > 0) {
		this->EliminateCycle(path, at_next_pos, flow);
		return true;
	}

	return false;
}

/**
 * Eliminate all cycles in the graph. Check paths starting at each node for
 * potential cycles.
 * @return If any cycles have been found and eliminated.
 */
bool MCF1stPass::EliminateCycles()
{
	bool cycles_found = false;
	uint size = this->job.Size();
	PathVector path(size, NULL);
	for (NodeID node = 0; node < size; ++node) {
		/* Starting at each node in the graph find all cycles involving this
		 * node. */
		std::fill(path.begin(), path.end(), (Path *)NULL);
		cycles_found |= this->EliminateCycles(path, node, node);
	}
	return cycles_found;
}

/**
 * Run the first pass of the MCF calculation.
 * @param job Link graph job to calculate.
 */
MCF1stPass::MCF1stPass(LinkGraphJob &job) : MultiCommodityFlow(job)
{
	PathVector paths;
	uint size = job.Size();
	uint accuracy = job.Settings().accuracy;
	bool more_loops;

	do {
		more_loops = false;
		for (NodeID source = 0; source < size; ++source) {
			/* First saturate the shortest paths. */
			this->Dijkstra<DistanceAnnotation, GraphEdgeIterator>(source, paths);

			for (NodeID dest = 0; dest < size; ++dest) {
				Edge edge = job[source][dest];
				if (edge.UnsatisfiedDemand() > 0) {
					Path *path = paths[dest];
					assert(path != NULL);
					/* Generally only allow paths that don't exceed the
					 * available capacity. But if no demand has been assigned
					 * yet, make an exception and allow any valid path *once*. */
					if (path->GetFreeCapacity() > 0 && this->PushFlow(edge, path,
							accuracy, this->max_saturation) > 0) {
						/* If a path has been found there is a chance we can
						 * find more. */
						more_loops = more_loops || (edge.UnsatisfiedDemand() > 0);
					} else if (edge.UnsatisfiedDemand() == edge.Demand() &&
							path->GetFreeCapacity() > INT_MIN) {
						this->PushFlow(edge, path, accuracy, UINT_MAX);
					}
				}
			}
			this->CleanupPaths(source, paths);
		}
	} while (more_loops || this->EliminateCycles());
}

/**
 * Run the second pass of the MCF calculation which assigns all remaining
 * demands to existing paths.
 * @param job Link graph job to calculate.
 */
MCF2ndPass::MCF2ndPass(LinkGraphJob &job) : MultiCommodityFlow(job)
{
	this->max_saturation = UINT_MAX; // disable artificial cap on saturation
	PathVector paths;
	uint size = job.Size();
	uint accuracy = job.Settings().accuracy;
	bool demand_left = true;
	while (demand_left) {
		demand_left = false;
		for (NodeID source = 0; source < size; ++source) {
			this->Dijkstra<CapacityAnnotation, FlowEdgeIterator>(source, paths);
			for (NodeID dest = 0; dest < size; ++dest) {
				Edge edge = this->job[source][dest];
				Path *path = paths[dest];
				if (edge.UnsatisfiedDemand() > 0 && path->GetFreeCapacity() > INT_MIN) {
					this->PushFlow(edge, path, accuracy, UINT_MAX);
					if (edge.UnsatisfiedDemand() > 0) demand_left = true;
				}
			}
			this->CleanupPaths(source, paths);
		}
	}
}

/**
 * Relation that creates a weak order without duplicates.
 * Avoid accidentally deleting different paths of the same capacity/distance in
 * a set. When the annotation is the same node IDs are compared, so there are
 * no equal ranges.
 * @tparam T Type to be compared on.
 * @param x_anno First value.
 * @param y_anno Second value.
 * @param x Node id associated with the first value.
 * @param y Node id associated with the second value.
 */
template <typename T>
bool Greater(T x_anno, T y_anno, NodeID x, NodeID y)
{
	if (x_anno > y_anno) return true;
	if (x_anno < y_anno) return false;
	return x > y;
}

/**
 * Compare two capacity annotations.
 * @param x First capacity annotation.
 * @param y Second capacity annotation.
 * @return If x is better than y.
 */
bool CapacityAnnotation::Comparator::operator()(const CapacityAnnotation *x,
		const CapacityAnnotation *y) const
{
	return x != y && Greater<int>(x->GetAnnotation(), y->GetAnnotation(),
			x->GetNode(), y->GetNode());
}

/**
 * Compare two distance annotations.
 * @param x First distance annotation.
 * @param y Second distance annotation.
 * @return If x is better than y.
 */
bool DistanceAnnotation::Comparator::operator()(const DistanceAnnotation *x,
		const DistanceAnnotation *y) const
{
	return x != y && !Greater<uint>(x->GetAnnotation(), y->GetAnnotation(),
			x->GetNode(), y->GetNode());
}