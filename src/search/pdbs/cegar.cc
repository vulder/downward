#include "cegar.h"

#include "pattern_database.h"
#include "steepest_ascent_enforced_hill_climbing.h"
#include "types.h"

#include "../option_parser.h"

#include "../tasks/projected_task.h"

#include "../task_utils/task_properties.h"

#include "../utils/countdown_timer.h"

using namespace std;

namespace pdbs {
class Projection {
    shared_ptr<PatternDatabase> pdb;
    vector<vector<OperatorID>> plan;
    bool unsolvable;
    bool solved;

public:
    Projection(
        const shared_ptr<PatternDatabase> &&pdb,
        const vector<vector<OperatorID>> &&plan,
        bool unsolvable)
        : pdb(move(pdb)),
          plan(move(plan)),
          unsolvable(unsolvable),
          solved(false) {}

    const shared_ptr<PatternDatabase> &get_pdb() const {
        return pdb;
    }

    const Pattern &get_pattern() const {
        return pdb->get_pattern();
    }

    const vector<vector<OperatorID>> &get_plan() const {
        return plan;
    }

    bool is_unsolvable() const {
        return unsolvable;
    }

    void mark_as_solved() {
        solved = true;
    }

    bool is_solved() {
        return solved;
    }
};

static unique_ptr<Projection> compute_projection(
    const shared_ptr<AbstractTask> &concrete_task,
    const Pattern &pattern,
    const shared_ptr<utils::RandomNumberGenerator> &rng,
    bool compute_wildcard_plan,
    utils::Verbosity verbosity) {
    TaskProxy concrete_task_proxy(*concrete_task);
    shared_ptr<PatternDatabase> pdb =
        make_shared<PatternDatabase>(concrete_task_proxy, pattern);
    tasks::ProjectedTask projected_task(concrete_task, pattern);
    TaskProxy projected_task_proxy(projected_task);

    bool unsolvable = false;
    vector<vector<OperatorID>> plan;
    int init_goal_dist =
        pdb->get_value_abstracted(projected_task_proxy.get_initial_state());
    if (init_goal_dist == numeric_limits<int>::max()) {
        unsolvable = true;
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << "PDB with pattern " << pattern
                         << " is unsolvable" << endl;
        }
    } else {
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << "Computing plan for PDB with pattern "
                         << pattern << endl;
        }

        plan = steepest_ascent_enforced_hillclimbing(
            projected_task_proxy, rng, *pdb, compute_wildcard_plan, verbosity);

        // Convert operator IDs of the abstract in the concrete task.
        for (vector<OperatorID> &plan_step : plan) {
            vector<OperatorID> concrete_plan_step;
            concrete_plan_step.reserve(plan_step.size());
            for (OperatorID abs_op_id : plan_step) {
                concrete_plan_step.push_back(
                    projected_task_proxy.get_operators()[abs_op_id].get_ancestor_operator_id(concrete_task.get()));
            }
            plan_step.swap(concrete_plan_step);
        }
    }

    return utils::make_unique_ptr<Projection>(
        move(pdb), move(plan), unsolvable);
}

struct Flaw {
    int collection_index;
    int variable;

    Flaw(int collection_index, int variable)
        : collection_index(collection_index),
          variable(variable) {
    }
};

using FlawList = vector<Flaw>;

class Cegar {
    const int max_refinements;
    const int max_pdb_size;
    const int max_collection_size;
    const bool wildcard_plans;
    const double max_time;
    const shared_ptr<AbstractTask> &task;
    const TaskProxy task_proxy;
    const vector<FactPair> goals;
    unordered_set<int> blacklisted_variables;
    shared_ptr<utils::RandomNumberGenerator> rng;
    const utils::Verbosity verbosity;
    const string token = "CEGAR: ";

    vector<unique_ptr<Projection>> projection_collection;
    /*
      Map each variable of the task which is contained in the collection to the
      projection which it is part of.
    */
    unordered_map<int, int> variable_to_projection;
    int collection_size;

    // Store the index of a projection if it solves the concrete task.
    int concrete_solution_index;

    void print_collection() const;
    void compute_initial_collection();
    bool time_limit_reached(const utils::CountdownTimer &timer) const;
    bool termination_conditions_met(
        const utils::CountdownTimer &timer, int refinement_counter) const;

    /*
      Try to apply the plan of the projection at the given index in the
      concrete task starting at the given state. During application,
      blacklisted variables are ignored. If plan application succeeds,
      return an empty flaw list and set concrete_solution_index if there
      are no blacklisted variables (in this case, the plan is a valid plan
      for the concrete task). Otherwise, return all precondition variables
      of all operators of the failing plan step.
     */
    FlawList apply_wildcard_plan(int collection_index, const State &init);
    FlawList get_flaws();

    // Methods related to refining.
    void add_pattern_for_var(int var);
    bool can_merge_patterns(int index1, int index2) const;
    void merge_patterns(int index1, int index2);
    bool can_add_variable_to_pattern(int index, int var) const;
    void add_variable_to_pattern(int collection_index, int var);
    void handle_flaw(const Flaw &flaw);
    void refine(const FlawList &flaws);
public:
    Cegar(
        int max_refinements,
        int max_pdb_size,
        int max_collection_size,
        bool wildcard_plans,
        double max_time,
        const shared_ptr<AbstractTask> &task,
        vector<FactPair> &&goals,
        unordered_set<int> &&blacklisted_variables,
        const shared_ptr<utils::RandomNumberGenerator> &rng,
        utils::Verbosity verbosity);

    PatternCollectionInformation run();
};

void Cegar::print_collection() const {
    utils::g_log << "[";
    for (size_t i = 0; i < projection_collection.size(); ++i) {
        const unique_ptr<Projection> &projection = projection_collection[i];
        if (projection) {
            utils::g_log << projection->get_pattern();
            if (i != projection_collection.size() - 1) {
                utils::g_log << ", ";
            }
        }
    }
    utils::g_log << "]" << endl;
}

void Cegar::compute_initial_collection() {
    assert(!goals.empty());
    for (const FactPair &goal : goals) {
        add_pattern_for_var(goal.var);
    }

    if (verbosity >= utils::Verbosity::VERBOSE) {
        utils::g_log << token << "initial collection: ";
        print_collection();
        utils::g_log << endl;
    }
}

bool Cegar::time_limit_reached(
    const utils::CountdownTimer &timer) const {
    if (timer.is_expired()) {
        if (verbosity >= utils::Verbosity::NORMAL) {
            utils::g_log << token << "time limit reached" << endl;
        }
        return true;
    }
    return false;
}

bool Cegar::termination_conditions_met(
    const utils::CountdownTimer &timer, int refinement_counter) const {
    if (time_limit_reached(timer)) {
        return true;
    }

    if (refinement_counter == max_refinements) {
        if (verbosity >= utils::Verbosity::NORMAL) {
            utils::g_log << token << "maximum allowed number of refinements reached." << endl;
        }
        return true;
    }

    return false;
}

/*
  TODO: this is a duplicate of State::get_unregistered_successor.
  The reason is that we may apply operators even though they are not
  applicable due to ignoring blacklisted violated preconditions.
*/
State get_unregistered_successor(
    const shared_ptr<AbstractTask> &task,
    const State &state,
    const OperatorProxy &op) {
    assert(!op.is_axiom());
    vector<int> new_values = state.get_unpacked_values();

    for (EffectProxy effect : op.get_effects()) {
        if (does_fire(effect, state)) {
            FactPair effect_fact = effect.get_fact().get_pair();
            new_values[effect_fact.var] = effect_fact.value;
        }
    }

    assert(task->get_num_axioms() == 0);
    return State(*task, move(new_values));
}

FlawList Cegar::apply_wildcard_plan(int collection_index, const State &init) {
    FlawList flaws;
    State current(init);
    current.unpack();
    Projection &projection = *projection_collection[collection_index];
    const vector<vector<OperatorID>> &plan = projection.get_plan();
    for (const vector<OperatorID> &equivalent_ops : plan) {
        bool step_failed = true;
        for (OperatorID op_id : equivalent_ops) {
            OperatorProxy op = task_proxy.get_operators()[op_id];

            /*
              Check if the operator is applicable. If not, add its violated
              preconditions to the list of flaws.
            */
            bool flaw_detected = false;
            for (FactProxy precondition : op.get_preconditions()) {
                int var = precondition.get_variable().get_id();

                // Ignore blacklisted variables
                if (blacklisted_variables.count(var)) {
                    continue;
                }

                if (current[precondition.get_variable()] != precondition) {
                    flaw_detected = true;
                    flaws.emplace_back(collection_index, var);
                }
            }

            /*
              If the operator is applicable, clear flaws and proceed with
              the next operator.
            */
            if (!flaw_detected) {
                step_failed = false;
                flaws.clear();
                current = get_unregistered_successor(task, current, op);
                break;
            }
        }

        // If all equivalent operators are inapplicable, stop plan execution.
        if (step_failed) {
            break;
        }
    }

    if (verbosity >= utils::Verbosity::VERBOSE) {
        utils::g_log << token << "plan of pattern " << projection.get_pattern();
    }
    if (flaws.empty()) {
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << " successfully executed ";
        }
        if (task_properties::is_goal_state(task_proxy, current)) {
            /*
              If there are no flaws, this does not guarantee that the plan
              is valid in the concrete state space because we might have
              ignored variables that have been blacklisted. Hence the tests
              for empty blacklists.
            */
            if (verbosity >= utils::Verbosity::VERBOSE) {
                utils::g_log << "and resulted in a concrete goal state." << endl;
            }
            if (blacklisted_variables.empty()) {
                if (verbosity >= utils::Verbosity::VERBOSE) {
                    utils::g_log << token << "since there are no blacklisted variables, "
                        "the concrete task is solved." << endl;
                }
                concrete_solution_index = collection_index;
            } else {
                if (verbosity >= utils::Verbosity::VERBOSE) {
                    utils::g_log << token << "since there are blacklisted variables, the plan "
                        "is not guaranteed to work in the concrete state "
                        "space. Marking this projection as solved." << endl;
                }
                projection.mark_as_solved();
            }
        } else {
            if (verbosity >= utils::Verbosity::VERBOSE) {
                utils::g_log << "but did not lead to a goal state." << endl;
            }
            for (const FactPair &goal : goals) {
                int goal_var_id = goal.var;
                if (current[goal_var_id].get_pair() != goal && !blacklisted_variables.count(goal_var_id)) {
                    flaws.emplace_back(collection_index, goal_var_id);
                }
            }
            if (flaws.empty()) {
                if (verbosity >= utils::Verbosity::VERBOSE) {
                    utils::g_log << token
                                 << "no non-blacklisted goal variables left, "
                        "marking this pattern as solved." << endl;
                }
                projection.mark_as_solved();
            } else {
                if (verbosity >= utils::Verbosity::VERBOSE) {
                    utils::g_log << token << "raising goal violation flaw(s)." << endl;
                }
            }
        }
    } else {
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << " failed." << endl;
        }
    }

    return flaws;
}

FlawList Cegar::get_flaws() {
    FlawList flaws;
    State concrete_init = task_proxy.get_initial_state();

    for (size_t collection_index = 0;
         collection_index < projection_collection.size(); ++collection_index) {
        if (!projection_collection[collection_index] ||
            projection_collection[collection_index]->is_solved()) {
            continue;
        }

        const Projection &projection = *projection_collection[collection_index];
        if (projection.is_unsolvable()) {
            utils::g_log << token << "Problem unsolvable" << endl;
            utils::exit_with(utils::ExitCode::SEARCH_UNSOLVABLE);
        }

        FlawList new_flaws = apply_wildcard_plan(collection_index, concrete_init);
        if (concrete_solution_index != -1) {
            /*
              The plan of the projection at collection_index is valid in the
              concrete task. Return empty flaws to signal terminating.
            */
            assert(concrete_solution_index == static_cast<int>(collection_index));
            assert(new_flaws.empty());
            assert(blacklisted_variables.empty());
            flaws.clear();
            return flaws;
        }
        for (Flaw &flaw : new_flaws) {
            flaws.push_back(move(flaw));
        }
    }
    return flaws;
}

void Cegar::add_pattern_for_var(int var) {
    projection_collection.push_back(
        compute_projection(
            task, {var}, rng, wildcard_plans, verbosity));
    variable_to_projection[var] = projection_collection.size() - 1;
    collection_size += projection_collection.back()->get_pdb()->get_size();
}

bool Cegar::can_merge_patterns(int index1, int index2) const {
    int pdb_size1 = projection_collection[index1]->get_pdb()->get_size();
    int pdb_size2 = projection_collection[index2]->get_pdb()->get_size();
    if (!utils::is_product_within_limit(pdb_size1, pdb_size2, max_pdb_size)) {
        return false;
    }
    int added_size = pdb_size1 * pdb_size2 - pdb_size1 - pdb_size2;
    return collection_size + added_size <= max_collection_size;
}

void Cegar::merge_patterns(int index1, int index2) {
    // Merge projection at index2 into projection at index2.
    Projection &projection1 = *projection_collection[index1];
    Projection &projection2 = *projection_collection[index2];

    const Pattern &pattern2 = projection2.get_pattern();
    for (int var : pattern2) {
        variable_to_projection[var] = index1;
    }

    // Compute merged pattern.
    Pattern new_pattern = projection1.get_pattern();
    new_pattern.insert(new_pattern.end(), pattern2.begin(), pattern2.end());
    sort(new_pattern.begin(), new_pattern.end());

    // Store old pdb sizes.
    int pdb_size1 = projection_collection[index1]->get_pdb()->get_size();
    int pdb_size2 = projection_collection[index2]->get_pdb()->get_size();

    // Compute merged projection.
    unique_ptr<Projection> merged =
        compute_projection(
            task, new_pattern, rng, wildcard_plans, verbosity);

    // Update collection size.
    collection_size -= pdb_size1;
    collection_size -= pdb_size2;
    collection_size += merged->get_pdb()->get_size();

    // Clean up.
    projection_collection[index1] = move(merged);
    projection_collection[index2] = nullptr;
}

bool Cegar::can_add_variable_to_pattern(int index, int var) const {
    int pdb_size = projection_collection[index]->get_pdb()->get_size();
    int domain_size = task_proxy.get_variables()[var].get_domain_size();
    if (!utils::is_product_within_limit(pdb_size, domain_size, max_pdb_size)) {
        return false;
    }
    int added_size = pdb_size * domain_size - pdb_size;
    return collection_size + added_size <= max_collection_size;
}

void Cegar::add_variable_to_pattern(int collection_index, int var) {
    const Projection &projection = *projection_collection[collection_index];

    Pattern new_pattern(projection.get_pattern());
    new_pattern.push_back(var);
    sort(new_pattern.begin(), new_pattern.end());

    unique_ptr<Projection> new_projection =
        compute_projection(task, new_pattern, rng, wildcard_plans, verbosity);

    collection_size -= projection.get_pdb()->get_size();
    collection_size += new_projection->get_pdb()->get_size();

    variable_to_projection[var] = collection_index;
    projection_collection[collection_index] = move(new_projection);
}

void Cegar::handle_flaw(const Flaw &flaw) {
    int collection_index = flaw.collection_index;
    int var = flaw.variable;
    bool added_var = false;
    auto it = variable_to_projection.find(var);
    if (it != variable_to_projection.end()) {
        // Variable is contained in another pattern of the collection.
        int other_index = it->second;
        assert(other_index != collection_index);
        assert(projection_collection[other_index] != nullptr);
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << token << "var" << var << " is already in pattern "
                         << projection_collection[other_index]->get_pattern() << endl;
        }
        if (can_merge_patterns(collection_index, other_index)) {
            if (verbosity >= utils::Verbosity::VERBOSE) {
                utils::g_log << token << "merge the two patterns" << endl;
            }
            merge_patterns(collection_index, other_index);
            added_var = true;
        }
    } else {
        // Variable is not yet in the collection.
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << token << "var" << var
                         << " is not in the collection yet" << endl;
        }
        if (can_add_variable_to_pattern(collection_index, var)) {
            if (verbosity >= utils::Verbosity::VERBOSE) {
                utils::g_log << token << "add it to the pattern" << endl;
            }
            add_variable_to_pattern(collection_index, var);
            added_var = true;
        }
    }

    if (!added_var) {
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << token << "Could not add var/merge patterns due to size "
                "limits. Blacklisting." << endl;
        }
        blacklisted_variables.insert(var);
    }
}

void Cegar::refine(const FlawList &flaws) {
    assert(!flaws.empty());
    int random_flaw_index = (*rng)(flaws.size());
    const Flaw &flaw = flaws[random_flaw_index];

    if (verbosity >= utils::Verbosity::VERBOSE) {
        utils::g_log << token << "chosen flaw: pattern "
                     << projection_collection[flaw.collection_index]->get_pattern()
                     << " with a flaw on " << flaw.variable << endl;
    }
    handle_flaw(flaw);
}

PatternCollectionInformation Cegar::run() {
    utils::CountdownTimer timer(max_time);
    compute_initial_collection();
    int refinement_counter = 0;
    while (!termination_conditions_met(timer, refinement_counter)) {
        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << "iteration #" << refinement_counter + 1 << endl;
        }

        FlawList flaws = get_flaws();

        if (flaws.empty()) {
            if (verbosity >= utils::Verbosity::NORMAL) {
                if (concrete_solution_index != -1) {
                    utils::g_log << token
                                 << "task solved during computation of abstract projection_collection"
                                 << endl;
                } else {
                    utils::g_log << token
                                 << "Flaw list empty. No further refinements possible."
                                 << endl;
                }
            }
            break;
        }

        if (time_limit_reached(timer)) {
            break;
        }

        refine(flaws);
        ++refinement_counter;

        if (verbosity >= utils::Verbosity::VERBOSE) {
            utils::g_log << token << "current collection size: " << collection_size << endl;
            utils::g_log << token << "current collection: ";
            print_collection();
            utils::g_log << endl;
        }
    }
    if (verbosity >= utils::Verbosity::VERBOSE) {
        utils::g_log << endl;
    }

    shared_ptr<PatternCollection> patterns = make_shared<PatternCollection>();
    shared_ptr<PDBCollection> pdbs = make_shared<PDBCollection>();
    if (concrete_solution_index != -1) {
        const shared_ptr<PatternDatabase> &pdb =
            projection_collection[concrete_solution_index]->get_pdb();
        pdbs->push_back(pdb);
        patterns->push_back(pdb->get_pattern());
    } else {
        for (const unique_ptr<Projection> &projection : projection_collection) {
            if (projection) {
                const shared_ptr<PatternDatabase> &pdb = projection->get_pdb();
                pdbs->push_back(pdb);
                patterns->push_back(pdb->get_pattern());
            }
        }
    }

    if (verbosity >= utils::Verbosity::NORMAL) {
        utils::g_log << token << "computation time: " << timer.get_elapsed_time() << endl;
        utils::g_log << token << "number of iterations: " << refinement_counter << endl;
        utils::g_log << token << "final collection: " << *patterns << endl;
        utils::g_log << token << "final collection number of patterns: " << patterns->size() << endl;
        utils::g_log << token << "final collection summed PDB sizes: " << collection_size << endl;
    }

    PatternCollectionInformation pattern_collection_information(
        task_proxy, patterns);
    pattern_collection_information.set_pdbs(pdbs);
    return pattern_collection_information;
}

Cegar::Cegar(
    int max_refinements,
    int max_pdb_size,
    int max_collection_size,
    bool wildcard_plans,
    double max_time,
    const shared_ptr<AbstractTask> &task,
    vector<FactPair> &&goals,
    unordered_set<int> &&blacklisted_variables,
    const shared_ptr<utils::RandomNumberGenerator> &rng,
    utils::Verbosity verbosity)
    : max_refinements(max_refinements),
      max_pdb_size(max_pdb_size),
      max_collection_size(max_collection_size),
      wildcard_plans(wildcard_plans),
      max_time(max_time),
      task(task),
      task_proxy(*task),
      goals(move(goals)),
      blacklisted_variables(move(blacklisted_variables)),
      rng(rng),
      verbosity(verbosity),
      collection_size(0),
      concrete_solution_index(-1) {
}

PatternCollectionInformation cegar(
    int max_refinements,
    int max_pdb_size,
    int max_collection_size,
    bool wildcard_plans,
    double max_time,
    const shared_ptr<AbstractTask> &task,
    vector<FactPair> &&goals,
    unordered_set<int> &&blacklisted_variables,
    const shared_ptr<utils::RandomNumberGenerator> &rng,
    utils::Verbosity verbosity) {
#ifndef NDEBUG
    TaskProxy task_proxy(*task);
    for (const FactPair goal : goals) {
        bool is_goal = false;
        for (const FactProxy &task_goal : task_proxy.get_goals()) {
            if (goal == task_goal.get_pair()) {
                is_goal = true;
                break;
            }
        }
        if (!is_goal) {
            cerr << " Given goal is not a goal of the task" << endl;
            utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
        }
    }
#endif
    if (verbosity >= utils::Verbosity::NORMAL) {
        utils::g_log << "Options of the CEGAR algorithm for computing a pattern collection: " << endl;
        utils::g_log << "max refinements: " << max_refinements << endl;
        utils::g_log << "max pdb size: " << max_pdb_size << endl;
        utils::g_log << "max collection size: " << max_collection_size << endl;
        utils::g_log << "wildcard plans: " << wildcard_plans << endl;
        utils::g_log << "Verbosity: ";
        switch (verbosity) {
        case utils::Verbosity::SILENT:
            utils::g_log << "silent";
            break;
        case utils::Verbosity::NORMAL:
            utils::g_log << "normal";
            break;
        case utils::Verbosity::VERBOSE:
            utils::g_log << "verbose";
            break;
        case utils::Verbosity::DEBUG:
            utils::g_log << "debug";
            break;
        }
        utils::g_log << endl;
        utils::g_log << "max time: " << max_time << endl;
        utils::g_log << "blacklisted variables: ";
        if (blacklisted_variables.empty()) {
            utils::g_log << "none";
        } else {
            for (int var : blacklisted_variables) {
                utils::g_log << var << ", ";
            }
        }
        utils::g_log << endl;
    }

    Cegar cegar(
        max_refinements,
        max_pdb_size,
        max_collection_size,
        wildcard_plans,
        max_time,
        task,
        move(goals),
        move(blacklisted_variables),
        rng,
        verbosity);
    return cegar.run();
}

void add_cegar_options_to_parser(options::OptionParser &parser) {
    parser.add_option<int>(
        "max_refinements",
        "maximum allowed number of refinements",
        "infinity",
        Bounds("0", "infinity"));
    parser.add_option<int>(
        "max_pdb_size",
        "maximum allowed number of states in a pdb (not applied to initial "
        "goal variable pattern(s))",
        "1000000",
        Bounds("1", "infinity"));
    parser.add_option<int>(
        "max_collection_size",
        "limit for the total number of PDB entries across all PDBs (not "
        "applied to initial goal variable pattern(s))",
        "infinity",
        Bounds("1", "infinity"));
    parser.add_option<bool>(
        "wildcard_plans",
        "Make the algorithm work with wildcard rather than regular plans.",
        "true");
    parser.add_option<double>(
        "max_time",
        "maximum time in seconds for CEGAR pattern generation. "
        "This includes the creation of the initial PDB collection"
        " as well as the creation of the correlation matrix.",
        "infinity",
        Bounds("0.0", "infinity"));
}
}
