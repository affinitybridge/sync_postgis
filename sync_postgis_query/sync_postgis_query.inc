<?php

class syncPgQuery {

  /**
   * The database connection object to use for the query.
   * 
   * @var DatabaseConnection
   */
  private $connection;

  /**
   * The SRID to use for the query.
   *
   * @var string
   */
  private $srid = '4326';

  /**
   * The name of the entity table to use in the query.
   *
   * @var string
   */
  private $entity_table;

  /**
   * The name of the geometry table to use in the query.
   *
   * @var string
   */
  private $geom_table;

  /**
   * The name of the geometry column to use in the query.
   *
   * @var string
   */
  private $geom_column;

  /**
   * The method of execution of the query.
   *
   * We set this to 'print' if we don't actually want it to be executed but just
   * returned as sql.
   *
   * @var string
   */
  private $exec_method = 'execute';

  /**
   * The entity type this query is concerned with, necessary for joining on a
   * bundle-specific table.
   *
   * @var string
   */
  private $entity_type = NULL;

  /**
   * The bundle this query is concerned with, necessary for joining on a
   * bundle-specific table.
   *
   * @var string
   */
  private $bundle = NULL;

  /**
   * The select query object.
   *
   * @var SelectQuery
   */
  private $query;

  /**
   * The array of placeholders being used in the query.
   *
   * @var array
   */
  private $placeholders;

  /**
   * Holds the result set of the executed query.
   */
  private $ordererd_results;

  /**
   * Array of tables that have been joined to the query.
   *
   * @var array
   */
  private $joins = array();

  /**
   * The fields we want to return in the result set.
   *
   * @var array
   */
  private $return_fields = array();

  /**
   * The filter conditions for the query.
   *
   * @var array
   */
  private $filterConditions = array();

  /**
   * Constructor for a syngPgQuery.
   *
   * @param $connection
   *   The DatabaseConnection object to use
   *
   * @param $srid
   *   The SRID to use
   *
   * @param $entity_table
   *   The name of the entity table in the database
   *
   * @param $geom_table
   *   The name of the geometry table in the database
   *
   * @param $geom_column
   *   The name of the geometry column in the database
   */
  public function __construct(DatabaseConnection $connection, $srid = NULL, $entity_table = SYNC_PG_ENTITY_TABLE, $geom_table = SYNC_PG_GEOMETRY_TABLE, $geom_column = SYNC_PG_GEOMETRY_COLUMN) {

    $this->connection = $connection;
    $this->entity_table = $entity_table;
    $this->geom_table = $geom_table;
    $this->geom_column = $geom_column;

    if ($srid) {
      $this->setSrid($srid);
    }
    // Initialise a select query on the entity table.
    $this->query = $this->connection->select($this->entity_table);
  }

  /**
   * Sets the srid to use in this query. Our geometry column uses 4326, so if
   * anything else gets passed in here, we will use st_transform in our query
   * to transform to the desired geometry.
   *
   * @param $srid
   *   A string representing the SRID to use in the query, e.g. '900913'.
   */
  public function setSrid($srid) {
    $this->srid = $srid;
  }

  /**
   * Returns the entity type being used in this query.
   */
  public function getEntityType() {
    return $this->entity_type;
  }

  /**
   * Returns the bundle being used in this query.
   */
  public function getBundle() {
    return $this->bundle;
  }

  /**
   * Sets the $entity_type and $bundle properties on the query object, allowing
   * bundle tables to be used in the query.
   */
  public function filterByBundle($entity_type, $bundle) {
    $this->entity_type = $entity_type;
    $this->bundle = $bundle;
    if (!isset($this->filterConditions['entity'])) {
      $this->filterConditions['entity'] = array();
    }
    $this->filterConditions['entity'] += array(
      array(
        'column' => 'entity_type',
        'value' => $entity_type,
        'operator' => '=',
      ),
      array(
        'column' => 'bundle',
        'value' => $bundle,
        'operator' => '=',
      ),
    );
    return $this;
  }

  /**
   * Add a condition to the query.
   *
   * @param $name
   *   Usually the table name, unless it is a bundle table condition.
   *
   * @param $column
   *   The column name.
   *
   * @param $value
   *   The value to check for.
   *
   * @param $operator
   *   The operator for the filter condition.
   *
   * @return SyncPgQuery
   *   The called object.
   */
  public function filterCondition($name, $column, $value, $operator = '=') {
    $this->filterConditions[$name][] = array(
      'column' => $column,
      'value' => $value,
      'operator' => $operator,
    );
    return $this;
  }

  /**
   * Add a geometry-based condition to the query.
   *
   * @param $type
   *   The type of condition, e.g. 'within', 'intersects'.
   *
   * @param $geom
   *   A string of wkt representing the geometry to check against
   *
   * @return SyncPgQuery
   *   The called object.
   */
  public function geometryCondition($type, $geom) {
    $this->geometryConditions[] = array(
      'type' => $type,
      'value' => $geom,
    );
    return $this;
  }

  /**
   * Add fields to be returned from the query.
   *
   * @param $fields
   *   An array of arrays of field names keyed by type, where type corresponds
   *   to a class that is responsible for the table on which to join, whose
   *   values should be included in the result set.
   *
   * @return SyncPgQuery
   *   The called object.
   */
  public function addReturnFields($fields = array()) {
    if (empty($fields)) {
      return $this;
    }
    $this->return_fields = array_merge_recursive($this->return_fields, $fields);
    return $this;
  }

  /**
   * This is just a convenience method so that client code does not have to specify
   * the name of the geometry table and geometry column.
   */
  public function returnGeometry() {
    $this->return_fields['geometry'] = array(SYNC_PG_GEOMETRY_COLUMN);
    return $this;
  }

  /**
   * Execute the query.
   *
   * @return
   *   The result set as an array of row objects.
   */
  public function execute() {
    // Give a chance to other modules to alter the query.
    drupal_alter('sync_pg_query', $this);
    $this->altered = TRUE;

    // Execute the query using the correct callback.
    $result = $this->syncPgQuery();

    return $result;
  }

  /**
   * Builds the query.
   *
   * @return
   *   See syncPgQuery::execute().
   */
  public function syncPgQuery() {

    if (empty($this->return_fields)) {
      // Nothing has been specified to be returned by the query, so just return
      // eid, entity_type and bundle.
      $this->query->fields($this->entity_table, array('eid', 'entity_type', 'bundle'));
    }

    foreach ($this->return_fields as $type => $fields) {
      $table_info = $this->ensureTable($type);
      $this->query->fields($table_info['table_name'], $fields);
    }

    $placeholder_index = 0;
    foreach ($this->filterConditions as $type => $filters) {
      $table_info = $this->ensureTable($type);
      foreach ($filters as $condition) {
        $field = $condition['column'];
        $sql_field = "{$table_info['table_name']}." . $field;
        $this->query->condition($sql_field, $condition['value'], $condition['operator']);
        if (isset($condition['value'])) {
          $ph_key = ':db_condition_placeholder_' . $placeholder_index++;
          // Add information about this field to the placeholders array.
          $this->placeholders[$ph_key] = array(
            'table' => $table_info['table_name'],
            'field' => $field,
          );
        }
      }
    }

    if (!empty($this->geometryConditions)) {
      // TODO: this is currently limited to geometry conditions of the form e.g.
      // "ST_Within(geom1, geom2)" - need to make it more generic.
      $table_info = $this->ensureTable('geometry');
      foreach ($this->geometryConditions as $i => $condition) {
        $placeholder_key = ':geom' . '_' . $i;
        $this->query->where("St_{$condition['type']}(St_Transform({$this->geom_table}.{$this->geom_column}, {$this->srid}), St_SetSrid(St_GeomFromText($placeholder_key), {$this->srid}))", array($placeholder_key => $condition['value']));
        if (isset($condition['value'])) {
          // Add information about this field to the placeholders array.
          $this->placeholders[$placeholder_key] = array(
            'table' => $this->geom_table,
            'field' => $this->geom_column,
          );
        }
      }
    }
    // TODO: add ability to order the query.
    return $this->finishQuery();
  }

  /**
   * Finalises and executes the query.
   */
  function finishQuery() {
    // TODO: add ability to do range queries and count queries.
    $return = array();
    // Only execute the query if $exec_method is set to 'execute'.
    if ($this->exec_method == 'execute') {
      foreach ($this->query->execute() as $row) {
        $return[] = $row;
        $this->ordered_results[] = $row;
      }
    }
    return $return;
  }

  /**
   * Returns the string of sql for the query, with all placeholders replaced by
   * the actual values.
   *
   * @return string
   *   A string version of the query.
   */
  public function getSql() {

    // Setting the exec_method property to 'print' will prevent the query from
    // actually executing; the select query object will be stored in the query
    // property, so that we can print it out.
    $this->exec_method = 'print';
    $this->execute();

    // The schemas for the tables involved in the query will let us know whether
    // each field contains numeric data or not.
    $schemas = sync_postgis_get_base_schemas();
    if (isset($this->entity_type) && isset($this->bundle)) {
      $table_name = sync_postgis_table_name($this->entity_type, $this->bundle);
      $schemas = array_merge($schemas, array($table_name => sync_postgis_get_bundle_schemas($this->entity_type, $this->bundle)));
    }

    // Figure out whether each placeholder value should have quotes around it or
    // not.
    $arg_types = array();
    foreach ($this->placeholders as $key => $info) {
      $arg_types[$key] = 's';
      if (isset($schemas[$info['table']]['fields'][$info['field']])) {
        if (in_array($schemas[$info['table']]['fields'][$info['field']]['type'], array('int', 'decimal', 'float'))) {
          $arg_types[$key] = 'd';
        }
      }
    }

    // Running the query string through prepareQuery gets rid of the curly
    // braces around table names.
    $prepared = $this->connection->prepareQuery((string) $this->query);
    $querystring = $prepared->queryString;

    // Now match up our arguments against their placeholder names and replace
    // them into the string - this essentially throws away all the goodness
    // that protects against sql injection but there's no way around that if we
    // want to be able to print out the full sql without placeholders.
    $args = $this->query->getArguments();

    foreach ($args as $placeholder => $value) {
      $replacement = $arg_types[$placeholder] == 'd' ? $value : $this->connection->quote($value);
      $querystring = str_replace($placeholder, $replacement, $querystring);
    }
    return $querystring;
  }

  /**
   * Performs a geometry relationship query on two entities. Each entity can
   * either be passed in as an array with keys 'eid' and 'entity_type' or as an
   * array of values corresponding to ids in the entity_geom table. The latter
   * use case is for running a query on an entity during presave when it does
   * not yet have an entity id but its geometries have been added to PostGIS.
   *
   * @param $type
   *   The type of relationship to check for between the two entities, e.g.
   *   'intersects', 'within'.
   *
   * @param $item1
   *   The first item of the relationship, the syntax being e.g. "item1 intersects
   *   item2".
   *
   * @param $item2
   *   The second item of the relationship.
   *
   * @param distance
   *   If this is a buffer query, the buffer distance to check.
   *
   * @return
   *   TRUE if the two entities have the relationship in question, FALSE if not.
   */
  public function booleanRelQuery($type, $item1, $item2, $distance = NULL) {
    // Initialise a select query on the geometry table...
    $select_query = $this->connection->select($this->geom_table, 'a');
    // ... CROSS JOIN'd with itself.
    $select_query->addJoin('CROSS', $this->geom_table, 'b');

    // Build a snippet for the relationship expression.
    $snippet = "st_$type(a." . $this->geom_column . ", b." . $this->geom_column;
    // Certain geometry relationship queries, such as ST_DWithin, have a third
    // parameter, which is the buffer distance to check.
    if (is_numeric($distance)) {
      $snippet .= ", " . $distance;
    }
    $snippet .= ")";
    $select_query->addExpression($snippet, 'relation');

    // Set up the conditions. This will produce a WHERE clause like e.g.
    // "WHERE a.eid = 5 AND a.entity_type = 'node' AND b.id IN (1,2,3)"
    foreach (array('a' => 'item1', 'b' => 'item2') as $alias => $param) {
      if (isset(${$param}['eid'])) {
        $select_query->condition("$alias.eid", ${$param}['eid']);
        $select_query->condition("$alias.entity_type", ${$param}['entity_type']);
      }
      else {
        // We've been passed an array of ids for geoms that don't yet have their
        // eid set.
        $select_query->condition("$alias.id", ${$param}, 'IN');
      }
    }

    // Execute the query and return TRUE if any of the entities' geometries
    // intersect each other.
    foreach ($select_query->execute() as $row) {
      if ($row->relation == TRUE) {
        return TRUE;
      }
    }
    return FALSE;
  }

  /**
   * Ensure that the table required for fields or filters is joined to the query.
   *
   * @param $name
   *  Usually corresponds to the table name.
   *
   * @see sync_pg_query_get_queryables().
   */
  public function ensureTable($name) {
    if (!isset($this->joins[$name])) {
      // Get the relevant table info so we can do a table join, if needed.
      $table_info = sync_pg_query_get_queryables($name);
      
      if (isset($table_info['callback'])) {
        $table_info = call_user_func($table_info['callback'], $this);
      }
      // Store the table info in our joins array.
      $this->joins[$name] = $table_info;

      // This will only happen if it's the entity table.
      if (empty($table_info['columns'])) {
        return $table_info;
      }

      // Build up the string to pass as the join condition.
      $condition = '';
      $i = 0;
      foreach ($table_info['columns'] as $this_table_col => $entity_table_col) {
        if ($i) {
          $condition .= ' AND ';
        }
        $condition .= "{$table_info['table_name']}.$this_table_col = {$this->entity_table}.$entity_table_col";
        $i++;
      }
      // Add the join to our select query.
      $this->query->join($table_info['table_name'], $table_info['table_name'], $condition);
    }
    return $this->joins[$name];
  }

}

/**
 * Exception thrown if invalid fields or filters are added to a query.
 */
class syncPgQueryException extends Exception {}
