<?php

class syncPgQuery extends EntityFieldQuery {

  private $connection;
  
  private $srid = '4326';

  private $entity_table;

  private $geom_table;

  private $geom_column;

  private $exec_method = 'execute';

  private $query_type;

  private $entity_type;

  private $bundle;

  private $query;

  private $placeholders;

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
   * Returns the string of sql for the query, with all placeholders replaced by
   * the actual values.
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

  // We overrride this because with our schema, entity conditions are no different
  // than property conditions.
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

  public function addFields($fields = array()) {
    if (empty($fields)) {
      return $this;
    }
    if (!isset($this->entity_type) || !isset($this->bundle)) {
      throw new EntityFieldQueryException(t('For this query an entity type and bundle must be specified.'));
    }
    // TODO: implement this!
  }

  public function addGeometry() {
    // TODO: implement this!
  }

  /**
   * Determines the query callback to use for this entity query.
   *
   * @return
   *   A callback that can be used with call_user_func().
   */
  public function queryCallback() {
    // Use the override from $this->executeCallback. It can be set either
    // while building the query, or using hook_entity_query_alter().
    if (function_exists($this->executeCallback)) {
      return $this->executeCallback;
    }
    return array($this, 'propertyQuery');
  }

  public function propertyQuery() {
    $schemas = sync_postgis_get_base_schemas();
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

    $select_query->fields($base_table, array('eid', 'entity_type', 'bundle'));

    if (!empty($this->fieldConditions)) {
      if (!isset($this->entity_type) || !isset($this->bundle)) {
        throw new EntityFieldQueryException(t('For this query an entity type and bundle must be specified.'));
      }

      // Join on the relevant bundle table.
      $table_name = sync_postgis_table_name($this->entity_type, $this->bundle);
      $table_alias = $table_name;
      $select_query->join($table_name, $table_alias, "$table_alias.entity_type = entity.entity_type AND $table_alias.eid = entity.eid");
  
      // Add field conditions.
      foreach ($this->fieldConditions as $key => $condition) {
        $field = $condition['field'];
        // Add the specified condition.
        $sql_field = "$table_alias." . $field['field_name'];
        $this->addCondition($select_query, $sql_field, $condition);
        if (isset($condition['value'])) {
          $key = ':db_condition_placeholder_' . $placeholder_index++;
          // Add information about this field to the placeholders array.
          $this->placeholders[$key] = array(
            'table' => $table_name,
            'field' => $field['field_name'],
          );
        }
      }
    }

    if (!empty($this->geometryConditions)) {
      $select_query->join($this->geom_table, 'g', 'g.entity_type = entity.entity_type AND g.eid = entity.eid');
      foreach ($this->geometryConditions as $i => $condition) {
        $placeholder_key = ':geom' . '_' . $i;
        $select_query->where("St_{$condition['type']}(St_Transform(g.geom, {$this->srid}), St_GeomFromText($placeholder_key))", array($placeholder_key => $condition['value']));
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

  public function fieldQuery() {
    return $this->propertyQuery();
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
    if ($this->exec_method == 'execute') {
      foreach ($select_query->execute() as $partial_entity) {
        $bundle = isset($partial_entity->bundle) ? $partial_entity->bundle : NULL;
        $return[$partial_entity->entity_type][$partial_entity->eid] = $partial_entity;
        $this->ordered_results[] = $partial_entity;
      }
    }
    else {
      // Store the select query object so that it can be printed out as sql.
      $this->query = $select_query;
    }
    return $return;
  }

  // Set the srid to use in this query. Our geometry column uses 4326, so if
  // anything else gets passed in here, we will use st_transform in our query
  // to transform to the desired geometry.
  public function setSrid($srid) {
    $this->srid = $srid;
  }

}
