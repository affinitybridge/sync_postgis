<?php

/**
 * Control which content types get their geofields sync'd.
 */
function sync_postgis_admin_settings() {
  $instances = field_info_instances();
  $form = array();
  foreach ($instances as $entity_type => $bundles) {
    foreach ($bundles as $bundle_name => $fields) {
      $options = array();
      $other_fields = array();
      foreach ($fields as $name => $info) {
        if ($info['widget']['module'] == 'geofield') {
          $options[$name] = $info['label'];
        }
        else {
          $other_fields[$name] = $info['label'];
        }
      }
      if (!empty($options)) {
        $form['sync_pg_' . $entity_type . '_' . $bundle_name] = array(
          '#markup' => '<h2>' . $bundle_name . '</h2>',
        );
        $form['sync_pg_enabled_' . $entity_type . '_' . $bundle_name] = array(
          '#type' => 'checkboxes',
          '#title' => t('Enable PostGIS syncing for these fields'),
          '#options' => $options,
          '#default_value' => variable_get('sync_pg_enabled_' . $entity_type . '_' . $bundle_name, array()),
          '#description' => t('Check which fields you would like sync\'d to PostGIS for this bundle'),
        );
        $form['sync_pg_fields_' . $entity_type . '_' . $bundle_name] = array(
          '#type' => 'checkboxes',
          '#title' => t('Additional data to store'),
          '#options' => $other_fields,
          '#default_value' => variable_get('sync_pg_fields_' . $entity_type . '_' . $bundle_name, array()),
          '#description' => t('Check which additional (non-geo) fields you would like to have in PostGIS for filtering or sorting on'),
        );
      }
    }

  }
  return system_settings_form($form);
}

/**
 * Bulk sync form.
 */
function sync_postgis_bulk_sync($form, &$form_state) {
  $form = array();
  $form['bulk_sync'] = array(
    '#type' => 'markup',
    '#markup' => '<div>' . t('Clicking the button below will cause all exiting content with geofields configured for syncing to be syncd to the PostGIS db.') . '</div>',
  );
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Sync all PostGis content'),
  );
  return $form;
}

/**
 * Bulk sync form.
 */
function sync_postgis_bulk_sync_submit($form, &$form_state) {
  $instances = field_info_instances();
  $nodes = array();
  $queue = DrupalQueue::get('sync_postgis_queue');
  foreach ($instances as $entity_type => $bundles) {
    foreach ($bundles as $bundle_name => $fields) {
      $enabled = array_filter(variable_get('sync_pg_enabled_' . $entity_type . '_' . $bundle_name, array()));
      if (!empty($enabled)) {
        // Add all entities of this type for processing.
        try {
          $query = new EntityFieldQuery();
          $entities = $query
            ->entityCondition('entity_type', $entity_type)
            ->entityCondition('bundle', $bundle_name)
            ->execute();

          $result = $entities[$entity_type];
          foreach ($result as $eid => $obj) {
            $item = array(
              'entity_type' => $entity_type,
              'bundle' => $bundle_name,
              'eid' => $eid,
            );
            $queue->createItem($item);
          }
        }
        catch (Exception $e) {
          watchdog_exception('sync_postgis_bulk', $e);
        }
  
      }
    }
  }
  drupal_set_message(t('All relevant content has been queued for syncing to the PostGIS db.'));
}

/**
 * Set up PostGIS base tables and configure required st_transform indexes on the
 * geometry column.
 * TODO: figure out a better way to organize this stuff, i.e. without having two
 * submit buttons :-/
 */
function sync_postgis_check_db_form($form, &$form_state) {
  $form['sync_postgis_transform_indexes'] = array(
    '#type' => 'textfield',
    '#title' => 'projections',
    '#description' => t('Comma-separated list of SRIDs for which you wish to have a ST_Transform index on the geom column'),
    '#default_value' => variable_get('sync_postgis_transform_indexes'),
  );
  $form['create_table'] = array(
    '#type' => 'submit',
    '#value' => 'Ensure PostGIS tables and indexes exist',
    '#submit' => array('sync_postgis_check_db_form_submit'),
  );
  return system_settings_form($form);
}

/**
 * Submit handler for the DB Set-up form. Ensures the base postgis tables exist
 * plus any additional st_transform indexes on the geometry column.
 */
function sync_postgis_check_db_form_submit($form, &$form_state) {
  $schemas = sync_postgis_get_base_schemas();
  $connection = sync_postgis_get_postgis_connection();
  // Set up the entity and entity_geom tables.
  foreach ($schemas as $table_name => $schema) {
    if (!$connection->schema()->tableExists($table_name)) {
      try {
        sync_postis_create_table($table_name, $schema);
        drupal_set_message(t('Created table @table', array('@table' => $table_name)));
      }
      catch (Exception $e) {
        drupal_set_message(t('Could not create table @table', array('@table' => $table_name)), 'error');
        watchdog_exception('sync_postgis_tables', $e);
      }
    }
    else {
      drupal_set_message(t('Table @table already exists', array('@table' => $table_name)));
    }
  }
  // Set up whatever st_transform indexes are required.
  $srids = explode(',', variable_get('sync_postgis_transform_indexes', ''));
  if (!empty($srids)) {
    foreach ($srids as $srid) {
      $srid = trim($srid);
      try {
        sync_postgis_add_index('entity_geom', 'transform_' . $srid, 'gist', 'ST_Transform(geom, ' . $srid . ')');
      }
      catch (Exception $e) {
        drupal_set_message($e->getMessage(), 'error');
      }
    }
  }
}

/**
 * Ensure we can connect to PostGIS and our table exists.
 */
function sync_postgis_setup_tables_form($form, &$form_state) {
  $schemas = sync_postgis_get_bundle_schemas();

  $form['description'] = array(
    '#type' => 'markup',
    '#markup' => '<div>' . t('Clicking the button below will cause any non-existent tables to be created in PostGIS, as per the configuration settings for each bundle.') . '</div>',
  );
  $form['create_tables'] = array(
    '#type' => 'submit',
    '#value' => 'Create tables',
  );
  return $form;
}

function sync_postgis_setup_tables_form_submit($form, &$form_state) {

  $schemas = sync_postgis_get_bundle_schemas();
  $db_field_types = sync_postgis_db_field_types();
  // Now create our postgis tables based on the schemas.
  $connection = sync_postgis_get_postgis_connection();
  foreach ($schemas as $entity_type => $bundles) {
    foreach ($bundles as $bundle_name => $schema) {
      $table_name = sync_postgis_table_name($entity_type, $bundle_name);
      // TODO: this should drop the table if it exists as the schema may have
      // changed.
      if (!$connection->schema()->tableExists($table_name)) {
        $schema_base = array(
          'fields' => array(
            'eid' => array(
              'description' => 'The id of the entity that this information belongs to.'
            ) + $db_field_types['integer'],
            'entity_type' => array(
              'description' => 'The type of entity'
            ) + $db_field_types['text'],
          ),
          // The eid + entity_type is a foreign key to the entity table but it is
          // also the primary key. This arises from the fact that we're creating
          // creating one-to-one table relationships. The only reason we're
          // doing it like this is to keep the bundle-specific data in
          // separate tables.
          'primary key' => array('eid', 'entity_type'),
          'foreign keys' => array(
            'entity_id' => array(
              'table' => 'entity',
              'columns' => array('eid' => 'eid', 'entity_type' => 'entity_type'),
            ), 
          ), 
        );
        $schema = array_merge_recursive($schema_base, $schema);
        try {
          sync_postis_create_table($table_name, $schema);
          drupal_set_message(t('Created table @table', array('@table' => $table_name)));
        }
        catch (Exception $e) {
          drupal_set_message(t('Could not create table @table', array('@table' => $table_name)), 'error');
          watchdog_exception('sync_postgis_tables', $e);
        }
      }
    }
  }
}