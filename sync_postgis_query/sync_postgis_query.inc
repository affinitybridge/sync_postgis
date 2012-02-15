<?php

class syncPgQuery extends EntityFieldQuery {

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
  private $entity_type;

  /**
   * The bundle this query is concerned with, necessary for joining on a
   * bundle-specific table.
   *
   * @var string
   */
  private $bundle;

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
   * The fields we want to return in the result set.
   *
   * @var array
   */
  private $return_fields = array();

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
   * Overrride this because with our schema, entity conditions are no different
   * from property conditions.
   */
  public function entityCondition($name, $value, $operator = NULL) {
    $this->propertyConditions[] = array(
      'column' => $name,
      'value' => $value,
      'operator' => $operator,
    );
    return $this;
  }

  public function fieldCondition($field, $column = NULL, $value = NULL, $operator = NULL, $delta_group = NULL, $language_group = NULL) {
    if (is_scalar($field)) {
      $field = array('field_name' => $field);
    }
    // Ensure the same index is used for fieldConditions as for fields.
    $index = count($this->fields);
    $this->fields[$index] = $field;
    if (isset($column)) {
      $this->fieldConditions[$index] = array(
        'field' => $field,
        'column' => $column,
        'value' => $value,
        'operator' => $operator,
        'delta_group' => $delta_group,
      );
    }
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

  public function entityOrderBy($name, $direction = 'ASC') {
    $this->order[] = array(
      'type' => 'property',
      'specifier' => $name,
      'direction' => $direction,
    );
    return $this;
  }

  public function fieldOrderBy($field, $column, $direction = 'ASC') {
    if (is_scalar($field)) {
      $field = array('field_name' => $field);
    }
    // Save the index used for the new field, for later use in field storage.
    $index = count($this->fields);
    $this->fields[$index] = $field;
    $this->order[] = array(
      'type' => 'field',
      'specifier' => array(
        'field' => $field,
        'index' => $index,
        'column' => $column,
      ),
      'direction' => $direction,
    );
    return $this;
  }

  /**
   * Add entity properties to be returned from the query.
   *
   * @param $columns
   *   An array of columns in the entity table whose values should be included
   *   in the result set.
   *
   * @return SyncPgQuery
   *   The called object.
   */
  public function addProperties($columns = array()) {
    if (empty($columns)) {
      return $this;
    }

    if (!isset($this->return_fields['entity_properties'])) {
      $this->return_fields['entity_properties'] = array();
    }
    $this->return_fields['entity_properties'] += $columns;
    return $this;
  }

  /**
   * Add bundle fields to be returned from the query.
   *
   * @param $fields
   *   An array of columns in the bundle table whose values should be included
   *   in the result set.
   *
   * @return SyncPgQuery
   *   The called object.
   */
  public function addBundleFields($fields = array()) {
    if (empty($fields)) {
      return $this;
    }

    if (!isset($this->return_fields['bundle_fields'])) {
      $this->return_fields['bundle_fields'] = array();
    }
    $this->return_fields['bundle_fields'] += $fields;
    return $this;
  }

  /**
   * Add the geometry to the set of fields to be returned from the query.
   *
   * @return SyncPgQuery
   *   The called object.
   */
  public function addGeometry() {
    if (!isset($this->return_fields['geometry'])) {
      $this->return_fields['geometry'] = array(SYNC_PG_GEOMETRY_COLUMN);
    }
    return $this;
  }

  /**
   * We only override the execute() method because the definition of the array
   * we return is different.
   *
   * @return
   *   The result set as an array of row objects.
   */
  public function execute() {
    $result = parent::execute();
    return $result;
  }

  /**
   * Overrides the queryCallback method to ensure that only the syncPgQuery
   * callback gets used.
   */
  public function queryCallback() {
    return array($this, 'syncPgQuery');
  }

  /**
   * This is the only query callback that will ever get used for a syncPgQuery,
   * unlike its parent class entityFieldQuery, which has multiple query callbacks
   * that can be used, depending on what conditions have been added.
   *
   * @return
   *   See syncPgQuery::execute().
   */
  public function syncPgQuery() {
    $base_table = $this->entity_table;

    // Initialise a select query on the entity table.
    $select_query = $this->connection->select($base_table);

    $placeholder_index = 0;
    // Process the property conditions.
    foreach ($this->propertyConditions as $condition) {
      $this->addCondition($select_query, "$base_table." . $condition['column'], $condition);
      if ($condition['column'] == 'entity_type') {
        $this->entity_type = $condition['value'];
      }
      elseif ($condition['column'] == 'bundle') {
        $this->bundle = $condition['value'];
      }
      if (isset($condition['value'])) {
        // Add information about this field to the placeholders array.
        $key = ':db_condition_placeholder_' . $placeholder_index++;
        $this->placeholders[$key] = array(
          'table' => $base_table,
          'field' => $condition['column'],
        );
      }
    }

    // Throw an error if we are trying to do anything bundle-specific in our
    // query but don't have entity type and bundle specified.
    if ((!empty($this->fieldConditions) || isset($this->return_fields['bundle_fields']))) {
      if (!isset($this->entity_type) || !isset($this->bundle)) {
        throw new EntityFieldQueryException(t('For this query an entity type and bundle must be specified.'));
      }
      $bundle_table_name = sync_postgis_table_name($this->entity_type, $this->bundle);
    }

    if (empty($this->return_fields)) {
      $select_query->fields($base_table, array('eid', 'entity_type', 'bundle'));
    }
    else {
      $table_names = array(
        'geometry' => SYNC_PG_GEOMETRY_TABLE,
        'entity_properties' => SYNC_PG_ENTITY_TABLE,
      );
      if (isset($bundle_table_name)) {
        $table_names['bundle_fields'] = $bundle_table_name;
      }
      foreach ($this->return_fields as $type => $fields) {
        $table = $table_names[$type];
        foreach ($fields as $field) {
          $select_query->fields($table, $fields);
        }
      }
    }

    if (!empty($this->fieldConditions)) {
      // Join on the relevant bundle table.
      $select_query->join($bundle_table_name, $bundle_table_name, "$bundle_table_name.entity_type = entity.entity_type AND $bundle_table_name.eid = entity.eid");
  
      // Add field conditions.
      foreach ($this->fieldConditions as $key => $condition) {
        $field = $condition['field'];
        // Add the specified condition.
        $sql_field = "$bundle_table_name." . $field['field_name'];
        $this->addCondition($select_query, $sql_field, $condition);
        if (isset($condition['value'])) {
          $key = ':db_condition_placeholder_' . $placeholder_index++;
          // Add information about this field to the placeholders array.
          $this->placeholders[$key] = array(
            'table' => $bundle_table_name,
            'field' => $field['field_name'],
          );
        }
      }
    }

    if (!empty($this->geometryConditions)) {
      $select_query->join($this->geom_table, $this->geom_table, "{$this->geom_table}.entity_type = entity.entity_type AND {$this->geom_table}.eid = entity.eid");
      foreach ($this->geometryConditions as $i => $condition) {
        $placeholder_key = ':geom' . '_' . $i;
        $select_query->where("St_{$condition['type']}(St_Transform({$this->geom_table}.{$this->geom_column}, {$this->srid}), St_GeomFromText($placeholder_key))", array($placeholder_key => $condition['value']));
        if (isset($condition['value'])) {
          // Add information about this field to the placeholders array.
          $this->placeholders[$placeholder_key] = array(
            'table' => $this->geom_table,
            'field' => $this->geom_column,
          );
        }
      }
    }

    // Order the query.
    foreach ($this->order as $order) {
      if ($order['type'] == 'property') {
        $select_query->orderBy("$base_table." . $order['specifier'], $order['direction']);
      }
      elseif ($order['type'] == 'field') {
        $specifier = $order['specifier'];
        $field = $specifier['field'];
        $sql_field = "$table_alias." . $field['field_name'];
        $select_query->orderBy($sql_field, $order['direction']);
      }
    }

    return $this->finishQuery($select_query);
  }

  /**
   * Override this function because we only have one query callback that should
   * ever be used.
   */
  public function propertyQuery() {
    return $this->queryCallback();
  }

  /**
   * Override this function because we only have one query callback that should
   * ever be used.
   */
  public function fieldQuery() {
    return $this->queryCallback();
  }

  function finishQuery($select_query, $id_key = 'eid') {
    foreach ($this->tags as $tag) {
      $select_query->addTag($tag);
    }
    foreach ($this->metaData as $key => $object) {
      $select_query->addMetaData($key, $object);
    }
    $select_query->addMetaData('entity_field_query', $this);
    if ($this->range) {
      $select_query->range($this->range['start'], $this->range['length']);
    }
    if ($this->count) {
      return $select_query->countQuery()->execute()->fetchField();
    }
    $return = array();

    // Execute of the query will either result in a result set being returned or
    // in the $query property being set, depending on what the $exec_method is
    // set to.
    if ($this->exec_method == 'execute') {
      foreach ($select_query->execute() as $row) {
        $return[] = $row;
        $this->ordered_results[] = $row;
      }
    }
    else {
      // Store the select query object so that it can be printed out as sql.
      $this->query = $select_query;
    }
    return $return;
  }



}
