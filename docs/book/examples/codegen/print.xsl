<?xml version="1.0" encoding="UTF-8"?>
<!-- Prints the tree of a TypeScript program as TypeScript text:
     one template rule for each kind of element in the tree. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:print="https://example.org/print"
    exclude-result-prefixes="#all">

  <xsl:output method="text"/>
  <xsl:strip-space elements="*"/>
  <xsl:preserve-space elements="string comment doc regex"/>

  <!-- ===== Layout: the only rules that know about lines and indentation. -->

  <!-- One level of indentation. -->
  <xsl:param name="indent" as="xs:string" select="'  '"/>

  <!-- The indentation of a node: one level for each block it is in. -->
  <xsl:function name="print:indent" as="xs:string">
    <xsl:param name="node" as="element()"/>
    <xsl:sequence select="string-join(
      $node/ancestor::*[self::body or self::then or self::else or self::interface or self::object]
      ! $indent)"/>
  </xsl:function>

  <!-- Whether a block is written with braces: all but the then of an if
       that has one statement and no else, which stays on the line of the if. -->
  <xsl:function name="print:braces" as="xs:boolean">
    <xsl:param name="block" as="element()"/>
    <xsl:sequence select="not($block/self::then and count($block/*) = 1 and empty($block/../else))"/>
  </xsl:function>

  <!-- Declarations, statements, members and entries are lines of their own. -->
  <xsl:template match="module/* | body/* | then[print:braces(.)]/* | else/*
                       | interface/(property | method) | object/entry" priority="1">
    <xsl:value-of select="print:indent(.)"/>
    <xsl:next-match/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>

  <!-- An empty line before each declaration that follows another. -->
  <xsl:template match="module/*[not(self::import)][preceding-sibling::*]" priority="2">
    <xsl:text>&#10;</xsl:text>
    <xsl:next-match/>
  </xsl:template>

  <!-- A block: its lines between braces, or its one statement. -->
  <xsl:template match="body | then | else | object">
    <xsl:call-template name="print:block"/>
  </xsl:template>

  <xsl:template name="print:block">
    <xsl:choose>
      <xsl:when test="print:braces(.)">
        <xsl:text>{&#10;</xsl:text>
        <xsl:apply-templates select="* except doc"/>
        <xsl:value-of select="print:indent(.)"/>
        <xsl:text>}</xsl:text>
      </xsl:when>
      <xsl:otherwise>
        <xsl:apply-templates/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- A documentation comment, on the line before its declaration. -->
  <xsl:template match="doc">
    <xsl:text>/** </xsl:text>
    <xsl:value-of select="."/>
    <xsl:text> */&#10;</xsl:text>
    <xsl:value-of select="print:indent(..)"/>
  </xsl:template>

  <!-- ===== Files -->

  <!-- A program is a set of modules; a module on its own is printed as
       the principal result. -->
  <xsl:template match="program | module">
    <xsl:apply-templates/>
  </xsl:template>

  <!-- Each module of a program is a file. -->
  <xsl:template match="program/module">
    <xsl:result-document href="{@file}" method="text">
      <xsl:apply-templates/>
    </xsl:result-document>
  </xsl:template>

  <xsl:template match="comment">// <xsl:value-of select="."/></xsl:template>

  <xsl:template match="import">
    <xsl:text>import </xsl:text>
    <xsl:if test="@type-only">type </xsl:if>
    <xsl:text>{ </xsl:text>
    <xsl:value-of select="name" separator=", "/>
    <xsl:text> } from </xsl:text>
    <xsl:value-of select="print:string(@from)"/>
    <xsl:text>;</xsl:text>
  </xsl:template>

  <!-- ===== Declarations -->

  <xsl:template match="@export">export </xsl:template>

  <xsl:template match="type-alias">
    <xsl:apply-templates select="@export"/>
    <xsl:text>type </xsl:text>
    <xsl:value-of select="@name"/>
    <xsl:text> = </xsl:text>
    <xsl:apply-templates/>
    <xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="interface">
    <xsl:apply-templates select="doc, @export"/>
    <xsl:text>interface </xsl:text>
    <xsl:value-of select="@name"/>
    <xsl:text> </xsl:text>
    <xsl:call-template name="print:block"/>
  </xsl:template>

  <xsl:template match="property">
    <xsl:value-of select="@name"/>
    <xsl:if test="@optional">?</xsl:if>
    <xsl:text>: </xsl:text>
    <xsl:apply-templates/>
    <xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="method">
    <xsl:apply-templates select="doc"/>
    <xsl:value-of select="@name"/>
    <xsl:apply-templates select="." mode="signature"/>
    <xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="function">
    <xsl:apply-templates select="doc, @export"/>
    <xsl:if test="@async">async </xsl:if>
    <xsl:text>function </xsl:text>
    <xsl:value-of select="@name"/>
    <xsl:apply-templates select="." mode="signature"/>
    <xsl:text> </xsl:text>
    <xsl:apply-templates select="body"/>
  </xsl:template>

  <!-- The parameters and the return type of a function or method. -->
  <xsl:template match="*" mode="signature">
    <xsl:text>(</xsl:text>
    <xsl:for-each select="parameter" separator=", ">
      <xsl:apply-templates select="."/>
    </xsl:for-each>
    <xsl:text>)</xsl:text>
    <xsl:for-each select="returns">: <xsl:apply-templates/></xsl:for-each>
  </xsl:template>

  <xsl:template match="parameter">
    <xsl:value-of select="@name"/>
    <xsl:for-each select="*">: <xsl:apply-templates select="."/></xsl:for-each>
  </xsl:template>

  <!-- ===== Types -->

  <xsl:template match="type">
    <xsl:value-of select="@name"/>
    <xsl:if test="*">
      <xsl:text>&lt;</xsl:text>
      <xsl:for-each select="*" separator=", ">
        <xsl:apply-templates select="."/>
      </xsl:for-each>
      <xsl:text>&gt;</xsl:text>
    </xsl:if>
  </xsl:template>

  <xsl:template match="array-type"><xsl:apply-templates/>[]</xsl:template>

  <xsl:template match="union">
    <xsl:for-each select="*" separator=" | ">
      <xsl:apply-templates select="."/>
    </xsl:for-each>
  </xsl:template>

  <xsl:template match="predicate"><xsl:value-of select="@parameter"/> is <xsl:apply-templates/></xsl:template>

  <!-- ===== Statements -->

  <xsl:template match="statement"><xsl:apply-templates/>;</xsl:template>

  <xsl:template match="return">return <xsl:apply-templates/>;</xsl:template>

  <xsl:template match="const">const <xsl:value-of select="@name"/> = <xsl:apply-templates/>;</xsl:template>

  <xsl:template match="if">
    <xsl:text>if (</xsl:text>
    <xsl:apply-templates select="*[1]"/>
    <xsl:text>) </xsl:text>
    <xsl:apply-templates select="then"/>
    <xsl:if test="else"> else </xsl:if>
    <xsl:apply-templates select="else"/>
  </xsl:template>

  <xsl:template match="for-of">
    <xsl:text>for (const </xsl:text>
    <xsl:value-of select="@name"/>
    <xsl:text> of </xsl:text>
    <xsl:apply-templates select="*[1]"/>
    <xsl:text>) </xsl:text>
    <xsl:apply-templates select="body"/>
  </xsl:template>

  <!-- ===== Expressions -->

  <xsl:template match="name | number | boolean">
    <xsl:value-of select="."/>
  </xsl:template>

  <!-- A string literal: JSON's strings are JavaScript's. -->
  <xsl:function name="print:string" as="xs:string">
    <xsl:param name="text" as="xs:string"/>
    <xsl:sequence select="serialize($text, { 'method': 'json', 'escape-solidus': false() })"/>
  </xsl:function>

  <xsl:template match="string">
    <xsl:value-of select="print:string(.)"/>
  </xsl:template>

  <!-- A regular expression literal: a slash in it is escaped. -->
  <xsl:template match="regex">
    <xsl:text>/</xsl:text>
    <xsl:value-of select="replace(., '/', '\\/')"/>
    <xsl:text>/</xsl:text>
    <xsl:value-of select="@flags"/>
  </xsl:template>

  <xsl:template match="member"><xsl:apply-templates/>.<xsl:value-of select="@name"/></xsl:template>

  <xsl:template match="not">!<xsl:apply-templates/></xsl:template>

  <xsl:template match="await">await <xsl:apply-templates/></xsl:template>

  <!-- The symbol of each operator. The generator never puts one binary
       expression in another, so no parentheses are needed. -->
  <xsl:variable name="operators" select="{
    'plus': '+', 'less': '&lt;', 'greater': '>', 'at-most': '&lt;=', 'at-least': '>=',
    'equal': '===', 'not-equal': '!=='
  }"/>

  <xsl:template match="binary">
    <xsl:for-each select="*" separator=" {$operators(current()/@operator)} ">
      <xsl:apply-templates select="."/>
    </xsl:for-each>
  </xsl:template>

  <!-- A call: the function's name, or a method of the first child. -->
  <xsl:template match="call">
    <xsl:choose>
      <xsl:when test="@method">
        <xsl:apply-templates select="*[1]"/>
        <xsl:text>.</xsl:text>
        <xsl:value-of select="@method"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="@function"/>
      </xsl:otherwise>
    </xsl:choose>
    <xsl:if test="type">
      <xsl:text>&lt;</xsl:text>
      <xsl:for-each select="type" separator=", ">
        <xsl:apply-templates select="."/>
      </xsl:for-each>
      <xsl:text>&gt;</xsl:text>
    </xsl:if>
    <xsl:text>(</xsl:text>
    <xsl:for-each select="(if (@method) then tail(*) else *) except type" separator=", ">
      <xsl:apply-templates select="."/>
    </xsl:for-each>
    <xsl:text>)</xsl:text>
  </xsl:template>

  <xsl:template match="array">
    <xsl:text>[</xsl:text>
    <xsl:for-each select="*" separator=", ">
      <xsl:apply-templates select="."/>
    </xsl:for-each>
    <xsl:text>]</xsl:text>
  </xsl:template>

  <xsl:template match="arrow">
    <xsl:text>(</xsl:text>
    <xsl:for-each select="parameter" separator=", ">
      <xsl:apply-templates select="."/>
    </xsl:for-each>
    <xsl:text>) => </xsl:text>
    <xsl:apply-templates select="* except parameter"/>
  </xsl:template>

  <xsl:template match="entry"><xsl:value-of select="@name"/>: <xsl:apply-templates/>,</xsl:template>

  <!-- A kind of element the printer doesn't know is an error, not a guess. -->
  <xsl:template match="*">
    <xsl:sequence select="error(#print:unknown, 'The printer has no rule for ' || name())"/>
  </xsl:template>

</xsl:stylesheet>
