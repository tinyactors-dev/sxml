<?xml version="1.0" encoding="UTF-8"?>
<!-- Draws a laid-out diagram (the output of layout.xq) as SVG, in three
     layers: the edges, the nodes, and the edges' labels on top. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns="http://www.w3.org/2000/svg"
    expand-text="yes">

  <xsl:output method="xml" indent="yes"/>

  <xsl:key name="node" match="node" use="@id"/>

  <xsl:template match="/diagram">
    <svg width="{@width}" height="{@height}" viewBox="0 0 {@width} {@height}">
      <title>{@title}</title>
      <style>
        .nodes rect {{ fill: #eef3fb; stroke: #3b5b92; stroke-width: 1.5 }}
        .nodes .store rect {{ fill: #f6efe3; stroke: #8a6a2f }}
        .nodes text {{ font: 14px sans-serif; fill: #1b1b1b }}
        .edges line, .edges path {{ fill: none; stroke: #555555; stroke-width: 1.5 }}
        .labels text {{ font: 12px sans-serif; fill: #555555 }}
      </style>
      <defs>
        <marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5"
                markerWidth="8" markerHeight="8" orient="auto">
          <path d="M 0 0 L 10 5 L 0 10 z" fill="#555555"/>
        </marker>
      </defs>
      <g class="edges">
        <xsl:apply-templates select="edge" mode="edges"/>
      </g>
      <g class="nodes">
        <xsl:apply-templates select="node" mode="nodes"/>
      </g>
      <g class="labels">
        <xsl:apply-templates select="edge[@label]" mode="labels"/>
      </g>
    </svg>
  </xsl:template>

  <!-- A description without a layout: stop, and say what to do. -->
  <xsl:template match="/diagram[not(@width)]" priority="1">
    <xsl:message terminate="yes">{@title}: no layout; run layout.xq first</xsl:message>
  </xsl:template>

  <!-- The edges layer: a line, or the path of a loop. -->
  <xsl:template match="edge[@x1]" mode="edges">
    <line id="edge-{@from}-{@to}" x1="{@x1}" y1="{@y1}" x2="{@x2}" y2="{@y2}"
          marker-end="url(#arrow)"/>
  </xsl:template>

  <xsl:template match="edge" mode="edges">
    <path id="edge-{@from}-{@to}" d="{@d}" marker-end="url(#arrow)"/>
  </xsl:template>

  <!-- The nodes layer: a box and its label. Stores have rounder corners. -->
  <xsl:template match="node" mode="nodes">
    <xsl:param name="corner" select="6"/>
    <g id="node-{@id}" class="{(@kind, 'service')[1], @class}">
      <rect x="{@x}" y="{@y}" width="140" height="40" rx="{$corner}"/>
      <text x="{@x + 70}" y="{@y + 25}" text-anchor="middle">{@label}</text>
    </g>
  </xsl:template>

  <xsl:template match="node[@kind = 'store']" mode="nodes">
    <xsl:next-match>
      <xsl:with-param name="corner" select="20"/>
    </xsl:next-match>
  </xsl:template>

  <!-- The labels layer: at the middle of a line, or beside a loop. -->
  <xsl:template match="edge[@x1]" mode="labels">
    <text x="{(@x1 + @x2) idiv 2 + 6}" y="{(@y1 + @y2) idiv 2 + 4}">{@label}</text>
  </xsl:template>

  <xsl:template match="edge" mode="labels">
    <xsl:variable name="from" select="key('node', @from)"/>
    <text x="{$from/@x + 56}" y="{$from/@y + 60}">{@label}</text>
  </xsl:template>

</xsl:stylesheet>
