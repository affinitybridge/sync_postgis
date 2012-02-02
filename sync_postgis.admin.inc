<?php

/**
 * Control which content types get their geofields sync'd.
 */
function sync_postgis_admin_settings() {
  $instances = field_info_instances('node');
  $form = array();
  foreach ($instances as $bundle_name => $fields) {
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
      $form['sync_postgis_' . $bundle_name] = array(
        '#markup' => '<h2>' . $bundle_name . '</h2>',
      );
      $form['sync_postgis_enabled_' . $bundle_name] = array(
        '#type' => 'checkboxes',
        '#title' => t('Enable PostGIS syncing for these fields'),
        '#options' => $options,
        '#default_value' => variable_get('sync_postgis_enabled_' . $bundle_name, array()),
        '#description' => t('Check which fields you would like sync\'d to PostGIS for this content type'),
      );
      $form['sync_postgis_fields_' . $bundle_name] = array(
        '#type' => 'checkboxes',
        '#title' => t('Additional data to store'),
        '#options' => $other_fields,
        '#default_value' => variable_get('sync_postgis_fields_' . $bundle_name, array()),
        '#description' => t('Check which additional (non-geo) fields you would like to store in PostGIS for this content type'),
      );
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
  $instances = field_info_instances('node');
  $nodes = array();
  $queue = DrupalQueue::get('sync_postgis_queue');
  foreach ($instances as $bundle_name => $fields) {
    $enabled = array_filter(variable_get('sync_postgis_enabled_' . $bundle_name, array()));
    if (!empty($enabled)) {
      // Add all nodes of this type for processing.
      $query = db_select('node', 'n', array('fetch' => PDO::FETCH_ASSOC));
      $query->fields('n', array('nid', 'title'));
      $query->condition('n.type', $bundle_name);
      try {
        $result = $query->execute();
        foreach ($result as $row) {
          $queue->createItem($row);
        }
      }
      catch (Exception $e) {
        watchdog_exception('sync_postgis_bulk', $e);
      }

    }
  }
  drupal_set_message(t('All relevant content has been queued for syncing to the PostGIS db.'));
}

/**
 * Ensure we can connect to PostGIS and our table exists.
 */
function sync_postgis_setup_tables_form($form, &$form_state) {
  $schemas = sync_postgis_get_schemas();
  dpm($schemas);
  $form['description'] = array(
    '#type' => 'markup',
    '#markup' => '<div>' . t('Clicking the button below will cause any non-existent tables to be created in PostGIS, as defined using hook_sync_postgis_tables.') . '</div>',
  );
  $form['create_table'] = array(
    '#type' => 'submit',
    '#value' => 'Create tables',
  );
  return $form;
}

function sync_postgis_setup_tables_form_submit($form, &$form_state) {

  $schemas = sync_postgis_get_schemas();
  // Now create our postgis tables based on the schemas.
  db_set_active(SYNC_POSTGIS_DB);
  foreach ($schemas as $bundle_name => $schema) {
    $table_name = 'pg_node_' . $bundle_name;
    if (!db_table_exists($table_name)) {
      $schema_base = array(
        'fields' => array(
          'nid' => array(
          'description' => 'The node that this information belongs to.',
          'type' => 'int',
          'not null' => TRUE,
          'default' => 0,
          )
        ),
        // Our nid column is a foreign key to the pg_node table but it is also
        // the primary key. This is an usual set-up arising from the fact that
        // we're creating one-to-one table relationships. The only reason we're
        // doing it like this is to keep the content-type specific data in
        // separate tables.
        'primary key' => array('nid'),
        'foreign keys' => array(
          'node_nid' => array(
            'table' => 'pg_node', 
            'columns' => array('nid' => 'nid'),
          ), 
        ), 
      );
      $schema = array_merge_recursive($schema, $schema_base);
      try {
        db_create_table($table_name, $schema);
        drupal_set_message('um, yay?');
      }
      catch (Exception $e) {
        drupal_set_message('sorry, did not work', 'error');
        watchdog_exception('sync_postgis_tables', $e);
      }
    }
  }
  db_set_active();
}

/**
 * Ensure we can connect to PostGIS and our table exists.
 */
//function sync_postgis_check_db_form($form, &$form_state) {
//  db_set_active(SYNC_POSTGIS_DB);
//  if (!db_table_exists('node')) {
//    $form['create_table'] = array(
//      '#type' => 'submit',
//      '#value' => 'Create PostGis Node table',
//    );
//    db_set_active();
//  }
//  else {
//    db_set_active();
//    drupal_set_message('The node table already exists in your postgis database');
//  }
//  return $form;
//}
//
//function sync_postgis_check_db_form_submit($form, &$form_state) {
//  db_set_active(SYNC_POSTGIS_DB);
//  try {
//    if (!db_table_exists('node')) {
//      // create it
//      global $databases;
//      $pg_db = $databases[SYNC_POSTGIS_DB]['default']['database'];
//      $cmd = sprintf('psql -d %s -f %s', $pg_db, drupal_get_path('module', 'sync_postgis') . '/sync_postgis.postgres.sql');
//      // TODO: Figure out how to make this work!!
//      $output = shell_exec($cmd);
//    }
//  }
//  catch (Exception $e) {
//    watchdog_exception('sync_postgis', $e);
//  }
//  db_set_active();
//}