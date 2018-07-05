#include "UBPlanner.h"

#include <QFile>
#include <QDebug>
#include <QLineF>
#include <QElapsedTimer>
#include <QGeoCoordinate>
#include <QCoreApplication>

#include "UBConfig.h"

#include "Waypoint.h"

#include <ilcplex/ilocplex.h>

ILOSTLBEGIN

UBPlanner::UBPlanner(QObject *parent) : QObject(parent),
    m_file(""),
    m_dim(10),
    m_limit(1000000000),
    m_gap(0.01),
    m_lambda(1),
    m_gamma(1),
    m_kappa(1000000000),
    m_pcs(100)
{
    m_areas.clear();
    m_nodes.clear();
    m_agents.clear();
    m_depots.clear();

    m_agent_paths.clear();
}

QList<Waypoint*> UBPlanner::loadWaypoints(const QString &loadFile) {
    QList<Waypoint*> wps;

    QFile file(loadFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << QObject::tr("Not able to open the mission file!");
        return wps;
    }

    QTextStream in(&file);

    const QStringList &version = in.readLine().split(" ");

    if (version.length() < 2) {
        qWarning() << QObject::tr("Waypoint file is corrupt. Version not detectable");
        return wps;
    }

    int versionInt = version[2].toInt();

    if (!(version.size() == 3 && version[0] == "QGC" && version[1] == "WPL" && versionInt >= 110)) {
        qWarning() << QObject::tr("The waypoint file is version %1 and is not compatible").arg(versionInt);
    } else {
        int id = 0;
        while (!in.atEnd()) {
            Waypoint *t = new Waypoint();
            if(t->load(in)) {
                t->setId(id);
                wps.append(t);
                id++;
            } else {
                qWarning() << QObject::tr("The waypoint file is corrupted. Load operation only partly succesful.");
                break;
            }
        }
    }

    file.close();

    return wps;
}

void UBPlanner::storeWaypoints(const QString& storeFile, QList<Waypoint*>& wps) {
    QFile file(storeFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << QObject::tr("Not able to open the mission file!");
        return;
    }

    QTextStream out(&file);

    //write the waypoint list version to the first line for compatibility check
    out << "QGC WPL 110\r\n";

    for (int i = 0; i < wps.count(); i++) {
        wps[i]->setId(i);
        wps[i]->save(out);
    }

    file.close();
}

void UBPlanner::startPlanner() {
    QList<Waypoint*> wps = loadWaypoints(m_file);
    QList<Waypoint*>::const_iterator i = wps.begin();
    while (i != wps.end()) {
        if ((*i)->getAction() == MAV_CMD_NAV_TAKEOFF) {

            QPolygonF area;
            area << QPointF((*i)->getLatitude(), (*i)->getLongitude());

            QList<Waypoint*>::const_iterator j = i;
            while (j != wps.end()) {
                j++;

                area << QPointF((*j)->getLatitude(), (*j)->getLongitude());

                if ((*j)->getAction() == MAV_CMD_NAV_LAND) {
                    i = j;

                    area << area[0];
                    m_areas << area;

                    break;
                }
            }
        } else if ((*i)->getAction() == MAV_CMD_NAV_RETURN_TO_LAUNCH) {
            m_depots << 0;
            m_agents << QGeoCoordinate((*i)->getLatitude(), (*i)->getLongitude());
            m_agent_paths << QVector<QPair<quint32, quint32> >();
        }

        i++;
    }

    plan();
}

void UBPlanner::decompose() {
    QPointF sf = m_areas[0].boundingRect().bottomLeft();
    QPointF rf = m_areas[0].boundingRect().bottomRight();
    QPointF uf = m_areas[0].boundingRect().topLeft();

    QGeoCoordinate s(sf.x(), sf.y());
    QGeoCoordinate r(rf.x(), rf.y());
    QGeoCoordinate u(uf.x(), uf.y());

    qreal xazimuth = s.azimuthTo(r);
    qreal yazimuth = s.azimuthTo(u);

    qreal xstep = ceil(s.distanceTo(r) / m_dim);
    qreal ystep = ceil(s.distanceTo(u) / m_dim);
    for (int i = 0; i < ystep; i++) {
        for (int j = 0; j < xstep; j++) {
            QGeoCoordinate x0 = s.atDistanceAndAzimuth(j * m_dim, xazimuth);
            QGeoCoordinate y0 = s.atDistanceAndAzimuth(i * m_dim, yazimuth);
            QGeoCoordinate x1 = s.atDistanceAndAzimuth(j * m_dim, xazimuth);
            QGeoCoordinate y1 = s.atDistanceAndAzimuth((i + 1) * m_dim, yazimuth);
            QGeoCoordinate x2 = s.atDistanceAndAzimuth((j + 1) * m_dim, xazimuth);
            QGeoCoordinate y2 = s.atDistanceAndAzimuth((i + 1) * m_dim, yazimuth);
            QGeoCoordinate x3 = s.atDistanceAndAzimuth((j + 1) * m_dim, xazimuth);
            QGeoCoordinate y3 = s.atDistanceAndAzimuth(i * m_dim, yazimuth);

            QPointF xf0(x0.latitude(), x0.longitude());
            QPointF yf0(y0.latitude(), y0.longitude());
            QPointF xf1(x1.latitude(), x1.longitude());
            QPointF yf1(y1.latitude(), y1.longitude());
            QPointF xf2(x2.latitude(), x2.longitude());
            QPointF yf2(y2.latitude(), y2.longitude());
            QPointF xf3(x3.latitude(), x3.longitude());
            QPointF yf3(y3.latitude(), y3.longitude());

            QVector<QPointF> cell;
            cell << xf0 + yf0 - sf << xf1 + yf1 - sf << xf2 + yf2 - sf << xf3 + yf3 - sf << xf0 + yf0 - sf;

            if (evaluate(cell)) {
                QGeoCoordinate xm = s.atDistanceAndAzimuth((j + 0.5) * m_dim, xazimuth);
                QGeoCoordinate ym = s.atDistanceAndAzimuth((i + 0.5) * m_dim, yazimuth);

                m_nodes << QGeoCoordinate(xm.latitude() + ym.latitude() - s.latitude(), xm.longitude() + ym.longitude() - s.longitude());
            }
        }
    }
}

bool UBPlanner::evaluate(const QVector<QPointF>& cell) {
    for (int i = 0; i < cell.size() - 1; i++) {
        if (!m_areas[0].containsPoint(cell[i], Qt::OddEvenFill)) {
            return false;
        }

        for (int j = 1; j < m_areas.size(); j++) {
            if (m_areas[j].containsPoint(cell[i], Qt::OddEvenFill)) {
                return false;
            }
        }

        QLineF line(cell[i], cell[i + 1]);

        foreach (QPolygonF area, m_areas) {
            for (int k = 0; k < area.size() - 1; k++) {
                if (line.intersect(QLineF(area[k], area[k + 1]), NULL) == QLineF::BoundedIntersection) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool UBPlanner::divide() {
    bool result = false;

    IloEnv env;
    IloNumArray2 dist_agent_node(env);
    for (int a = 0; a < m_agents.size(); a++) {
        dist_agent_node.add(IloNumArray(env, m_nodes.size()));
    }

    for (int a = 0; a < m_agents.size(); a++) {
        for (int i = 0; i < m_nodes.size(); i++) {
            dist_agent_node[a][i] = m_agents[a].distanceTo(m_nodes[i]);
        }
    }

    try {
        IloModel mod(env);

        IloFloatVar z(env);
        IloArray<IloBoolVarArray> x_agent_node(env);
        for (int a = 0; a < m_agents.size(); a++) {
            x_agent_node.add(IloBoolVarArray(env, m_nodes.size()));
        }

        mod.add(IloMinimize(env, z));

        for (int a = 0; a < m_agents.size(); a++) {
            IloExpr total_dist(env);

            for (int i = 0; i < m_nodes.size(); i++) {
                total_dist += dist_agent_node[a][i] * x_agent_node[a][i];
            }

            mod.add(total_dist <= z);
            total_dist.end();
        }

        for (int i = 0; i < m_nodes.size(); i++) {
            IloExpr flow_in(env);

            for (int a = 0; a < m_agents.size(); a++) {
                flow_in += x_agent_node[a][i];
            }

            mod.add(flow_in == 1);
            flow_in.end();
        }

        IloCplex cplex(mod);
//        cplex.exportModel("div.lp");
        if (!cplex.solve()) {
            throw(-1);
        }

        result = true;

        env.out() << "Minimume Cost = " << cplex.getObjValue() << endl;

        for (int a = 0; a < m_agents.size(); a++) {
            for (int i = 0; i < m_nodes.size(); i++) {
                if (cplex.getValue(x_agent_node[a][i])) {
                    m_agent_paths[a] << QPair<quint32, quint32>(i, i);
                }
            }
        }
    }
    catch (IloException& e) {
        cerr << "Concert exception caught: " << e << endl;
    }
    catch (...) {
        cerr << "Unknown exception caught" << endl;
    }

    env.end();

    return result;
}

bool UBPlanner::planAgent(quint32 agent) {
    bool result = false;

    IloEnv env;

    m_depots[agent] = m_agent_paths[agent][0].first;

    qreal dist = 0;
    qreal min_dist = m_agents[agent].distanceTo(m_nodes[m_depots[agent]]);
    QPair<quint32, quint32> node;
    foreach (node, m_agent_paths[agent]) {
        dist = m_agents[agent].distanceTo(m_nodes[node.first]);

        if (dist < min_dist) {
            min_dist = dist;
            m_depots[agent] = node.first;
        }
    }

    IloIntArray2 dist_node_node(env);
    for (int i = 0; i < m_agent_paths[agent].size(); i++) {
        dist_node_node.add(IloIntArray(env, m_agent_paths[agent].size()));
    }

    IloIntArray3 direct_node_node_node(env);
    for (int i = 0; i < m_agent_paths[agent].size(); i++) {
        IloIntArray2 direct_node_node(env);
        for (int j = 0; j < m_agent_paths[agent].size(); j++) {
            direct_node_node.add(IloIntArray(env, m_agent_paths[agent].size()));
        }

        direct_node_node_node.add(direct_node_node);
    }

    qreal max_dist = (1.0 + sqrt(2.0) / 2.0) * m_dim;

    for (int i = 0; i < m_agent_paths[agent].size(); i++) {
        for (int j = 0; j < m_agent_paths[agent].size(); j++) {
            qreal dist = m_nodes[m_agent_paths[agent][i].first].distanceTo(m_nodes[m_agent_paths[agent][j].first]);

            if (!dist || dist > max_dist) {
                dist_node_node[i][j] = m_kappa;
            } else {
                dist_node_node[i][j] = m_pcs * dist;
            }
        }
    }

    for (int i = 0; i < m_agent_paths[agent].size(); i++) {
        for (int j = 0; j < m_agent_paths[agent].size(); j++) {
            for (int k = 0; k < m_agent_paths[agent].size(); k++) {
                if (dist_node_node[i][j] == m_kappa || dist_node_node[j][k] == m_kappa) {
                    direct_node_node_node[i][j][k] = 0;
                } else {
                    qreal r = m_nodes[m_agent_paths[agent][i].first].distanceTo(m_nodes[m_agent_paths[agent][j].first]);
                    qreal s = m_nodes[m_agent_paths[agent][j].first].distanceTo(m_nodes[m_agent_paths[agent][k].first]);
                    qreal t = m_nodes[m_agent_paths[agent][k].first].distanceTo(m_nodes[m_agent_paths[agent][i].first]);

                    direct_node_node_node[i][j][k] = m_pcs * (M_PI - acos((r + s - t) / sqrt(4.0 * r * s)));
                }
            }
        }
    }

    try {
        IloModel mod(env);

        IloNumVarArray u(env, m_agent_paths[agent].size(), 0.0, IloInfinity, ILOFLOAT);
        IloArray<IloBoolVarArray> x_node_node(env);
        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            x_node_node.add(IloBoolVarArray(env, m_agent_paths[agent].size()));
        }

        IloExpr total_dist(env), total_direct(env);

        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            for (int j = 0; j < m_agent_paths[agent].size(); j++) {
                if (j == i) {
                    continue;
                }

                total_dist += dist_node_node[i][j] * x_node_node[i][j];
            }
        }

        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            for (int j = 0; j < m_agent_paths[agent].size(); j++) {
                if (j == i || m_agent_paths[agent][j].first == m_depots[agent]) {
                    continue;
                }

                for (int k = 0; k < m_agent_paths[agent].size(); k++) {
                    if (k == j) {
                        continue;
                    }

                    total_direct += direct_node_node_node[i][j][k] * x_node_node[i][j] * x_node_node[j][k];
                }
            }
        }

        mod.add(IloMinimize(env, m_lambda * total_dist + m_gamma * total_direct));

        total_dist.end();
        total_direct.end();

        for (int j = 0; j < m_agent_paths[agent].size(); j++) {
            IloExpr flow_in(env);

            for (int i = 0; i < m_agent_paths[agent].size(); i++) {
                if (i == j) {
                    continue;
                }

                flow_in += x_node_node[i][j];
            }

            mod.add(flow_in == 1);
            flow_in.end();
        }

        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            IloExpr flow_out(env);

            for (int j = 0; j < m_agent_paths[agent].size(); j++) {
                if (j == i) {
                    continue;
                }

                flow_out += x_node_node[i][j];
            }

            mod.add(flow_out == 1);
            flow_out.end();
        }

        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            if (m_agent_paths[agent][i].first == m_depots[agent]) {
                continue;
            }

            for (int j = 0; j < m_agent_paths[agent].size(); j++) {
                if (m_agent_paths[agent][j].first == m_depots[agent] || j == i) {
                    continue;
                }

                mod.add(u[i] - u[j] + m_agent_paths[agent].size() * x_node_node[i][j] <= m_agent_paths[agent].size() - 1);
            }
        }

        IloCplex cplex(mod);
        cplex.setParam(IloCplex::EpGap, m_gap);
        cplex.setParam(IloCplex::TiLim, m_limit);
        if (!cplex.solve() || cplex.getObjValue() / m_pcs >= m_kappa) {
            throw(-1);
        }

        result = true;

        env.out() << "Minimume Cost = " << cplex.getObjValue() / m_pcs << endl;

        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            for (int j = 0; j < m_agent_paths[agent].size(); j++) {
                if (j == i) {
                    continue;
                }

                if (cplex.getValue(x_node_node[i][j])) {
                    m_agent_paths[agent][i].second = m_agent_paths[agent][j].first;

                    break;
                }
            }
        }
    }
    catch (IloException& e) {
        cerr << "Concert exception caught: " << e << endl;
    }
    catch (...) {
        cerr << "Unknown exception caught" << endl;
    }

    env.end();

    return result;
}

bool UBPlanner::pathInfo(quint32 agent) {
    qreal dist = 0;
    qreal direct = 0;

    quint32 ang1 = 0;
    quint32 ang2 = 0;
    quint32 ang3 = 0;

    quint32 i = m_depots[agent];
    quint32 j = m_depots[agent];
    quint32 k = m_depots[agent];

    for (int node = 0; node < m_agent_paths[agent].size(); node++) {
        if (m_agent_paths[agent][node].first == i) {
            j = m_agent_paths[agent][node].second;

            break;
        }
    }

    qreal max_d = (1.0 + sqrt(2.0) / 2.0) * m_dim;

    while (true) {
        qreal d = m_nodes[i].distanceTo(m_nodes[j]);
        if (d > max_d) {
            return false;
        }

        dist += d;

        if (j == m_depots[agent]) {
            break;
        }

        for (int node = 0; node < m_agent_paths[agent].size(); node++) {
            if (m_agent_paths[agent][node].first == j) {
                k = m_agent_paths[agent][node].second;

                break;
            }
        }

        qreal r = m_nodes[i].distanceTo(m_nodes[j]);
        qreal s = m_nodes[j].distanceTo(m_nodes[k]);
        qreal t = m_nodes[k].distanceTo(m_nodes[i]);

        qreal q = M_PI - acos((r + s - t) / sqrt(4.0 * r * s));

        direct += q;

        if (q > M_PI / 4.0 - M_PI / 8.0 && q < M_PI / 4.0 + M_PI / 8.0) {
            ang1++;
        } else if (q > M_PI / 2.0 - M_PI / 8.0 && q < M_PI / 2.0 + M_PI / 8.0) {
            ang2++;
        } else if (q > 3.0 * M_PI / 4.0 - M_PI / 8.0 && q < 3.0 * M_PI / 4.0 + M_PI / 8.0) {
            ang3++;
        }

        i = j;
        j = k;
    }

    cout << "Total Distance: " << dist << " | Number of 45' Turn: " << ang1 << " | Number of 90' Turn: " << ang2 << " | Number of 135' Turn: " << ang3 << endl;
    cout << "Total Cost: " << m_lambda * dist + m_gamma * direct << endl;

    return true;
}

void UBPlanner::missionAgent(quint32 agent) {
    QList<Waypoint*> wps;

    Waypoint* wp = new Waypoint();
//    wp.setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
    wp->setAcceptanceRadius(POINT_ZONE);
    wp->setLatitude(m_nodes[m_depots[agent]].latitude());
    wp->setLongitude(m_nodes[m_depots[agent]].longitude());
    wps.append(wp);

    wp = new Waypoint();
    wp->setAction(MAV_CMD_NAV_TAKEOFF);
    wp->setAcceptanceRadius(POINT_ZONE);
    wp->setLatitude(m_nodes[m_depots[agent]].latitude());
    wp->setLongitude(m_nodes[m_depots[agent]].longitude());
    wp->setAltitude(TAKEOFF_ALT);
    wps.append(wp);

    quint32 node = m_depots[agent];
    while (true) {
        for (int i = 0; i < m_agent_paths[agent].size(); i++) {
            if (m_agent_paths[agent][i].first == node) {
                node = m_agent_paths[agent][i].second;

                break;
            }
        }

        wp = new Waypoint();
        wp->setAction(MAV_CMD_NAV_WAYPOINT);
        wp->setAcceptanceRadius(POINT_ZONE);
        wp->setLatitude(m_nodes[node].latitude());
        wp->setLongitude(m_nodes[node].longitude());
        wp->setAltitude(TAKEOFF_ALT);
        wps.append(wp);

        if (node == m_depots[agent]) {
            break;
        }
    }

    wp = new Waypoint();
    wp->setAction(MAV_CMD_NAV_LAND);
    wp->setAcceptanceRadius(POINT_ZONE);
    wp->setLatitude(m_nodes[node].latitude());
    wp->setLongitude(m_nodes[node].longitude());
    wps.append(wp);

    storeWaypoints(tr("mission_%1.txt").arg(agent), wps);
}

void UBPlanner::plan() {
    QElapsedTimer total_time;
    total_time.start();

    decompose();

    if (!divide()) {
        cerr << "Unable to divide the area between agents!" << endl;
        exit(EXIT_FAILURE);
    }

    QElapsedTimer agent_time;
    for (int a = 0; a < m_agents.size(); a++) {
        agent_time.restart();
        if (!planAgent(a) || !pathInfo(a)) {
            cerr << "Unable to plan the coverage path for agent " << a << " !" <<endl;
            exit(EXIT_FAILURE);
        }

        cout << "Elapsed time for agent " << a << " is "<< agent_time.elapsed() / 1000.0 << endl;

        missionAgent(a);
    }

    emit planReady();

    cout << "The planner has successfully planned the mission for each agent in total time " << total_time.elapsed() / 1000.0 << endl;
    exit(EXIT_SUCCESS);
}
